// Co-op multiplayer: client side.
//
// The client never simulates.  It keeps a scratch world with the host's mods,
// mirrors whatever the host streams to it and sends every game action to the
// host instead of executing it.

#include "mp_session.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>

#include "activity_actor_definitions.h"
#include "avatar.h"
#include "calendar.h"
#include "character.h"
#include "debug.h"
#include "game.h"
#include "cata_utility.h"
#include "path_info.h"
#include "filesystem.h"
#include "game_constants.h"
#include "construction.h"
#include "crafting_gui.h"
#include "game_inventory.h"
#include "gun_mode.h"
#include "iexamine.h"
#include "input.h"
#include "item.h"
#include "itype.h"
#include "item_handling_util.h"
#include "line.h"
#include "map/map.h"
#include "map/mapbuffer.h"
#include "map/mapdata.h"
#include "map/submap.h"
#include "messages.h"
#include "mod_manager.h"
#include "map/field_type.h"
#include "monster.h"
#include "mtype.h"
#include "mp_common.h"
#include "mp_net.h"
#include "npc.h"
#include "overmap/omdata.h"
#include "options.h"
#include "output.h"
#include "overmap/overmapbuffer.h"
#include "pickup.h"
#include "pickup_token.h"
#include "player_activity.h"
#include "popup.h"
#include "ranged.h"
#include "recipe.h"
#include "string_formatter.h"
#include "string_input_popup.h"
#include "string_utils.h"
#include "translations.h"
#include "ui.h"
#include "ui_manager.h"
#include "trap.h"
#include "vehicle/vehicle.h"
#include "vehicle/vpart_position.h"
#include "weather/weather.h"
#include "world.h"
#include "worldfactory.h"

// Private parts of `game` the client needs (see mp_host.cpp for the host's).
struct mp_client_game_access {
    static void add_active_npc( game &g, const shared_ptr_fast<npc> &np ) {
        g.active_npc.push_back( np );
        np->get_mapbuffer().add_active_npc( np );
    }
    static void set_seed( game &g, unsigned int s ) {
        g.seed = s;
    }
    static void vertical_shift( game &g, int z_before, int z_after ) {
        g.vertical_shift( z_before, z_after );
    }
    static void reset_light_level( game &g ) {
        g.reset_light_level();
    }
    static bool handle_action( game &g ) {
        return g.handle_action();
    }
    static bool start_game( game &g ) {
        return g.start_game();
    }
};

namespace cata_mp::client
{

namespace
{

const std::string &scratch_world_name = client_world_name;
constexpr int connect_timeout_ms = 6000;
constexpr int handshake_timeout_ms = 15000;

struct welcome_info {
    std::string world;
    std::string host;
    unsigned int seed = 0;
    std::vector<std::string> mods;
    std::map<std::string, std::string> options;
    std::vector<std::string> players;
};

struct state_t {
    std::string address;
    uint16_t port = default_port;
    std::string password;
    std::string my_name;
    std::string host_name;
    bool in_game = false;
    bool my_turn = false;
    bool awaiting_ack = false;
    bool waiting = false;
    int seq = 0;
    int64_t turn_wait_started = 0;
    bool got_state = false;
    bool leaving = false;
    bool reconnecting = false;
    std::string end_reason;
    std::map<std::string, shared_ptr_fast<monster>> monsters;
    std::set<int> npc_ids;
    int host_id = 0;
    // The character is busy with a long action on the host.
    bool busy = false;
};

state_t S;
// Submaps this client had to stub out because it did not have them yet.
// Stubs are created by map loading worker threads, so guard the list.
std::mutex placeholders_mutex;
std::vector<tripoint_abs_sm> placeholders;

// ---- low level helpers ------------------------------------------------------

bool scratch_world_exists()
{
    return world_generator->has_world( scratch_world_name );
}

bool parse_address( const std::string &text, std::string &host, uint16_t &port )
{
    std::string t = trim( text );
    if( t.empty() ) {
        return false;
    }
    port = default_port;
    // [ipv6]:port is not supported; host:port or plain host.
    const size_t colon = t.rfind( ':' );
    if( colon != std::string::npos && t.find( ':' ) == colon ) {
        const int p = std::atoi( t.substr( colon + 1 ).c_str() );
        if( p <= 0 || p >= 65536 ) {
            return false;
        }
        port = static_cast<uint16_t>( p );
        t = t.substr( 0, colon );
    }
    host = t;
    return !host.empty();
}

// Waits (pumping the UI) for one of the given message types.  Returns the raw
// message, or nullopt on timeout/close; `err` explains why.
std::optional<std::string> wait_for_message( const std::vector<std::string> &types,
        int timeout_ms, const std::string &status, std::string &err )
{
    static_popup notice;
    notice.on_top( true );
    const int64_t deadline = net::now_ms() + timeout_ms;
    while( net::now_ms() < deadline ) {
        notice.message( "%s", status );
        ui_manager::redraw();
        refresh_display();
        inp_mngr.pump_events();
        net::event ev;
        while( net::poll( ev ) ) {
            if( ev.k == net::event::kind::closed ) {
                err = ev.data;
                return std::nullopt;
            }
            if( ev.k != net::event::kind::line ) {
                continue;
            }
            message m( ev.data );
            if( !m.valid() ) {
                continue;
            }
            if( m.type() == "error" || m.type() == "bye" ) {
                JsonObject jo = m.object();
                err = jo.get_string( "msg", _( "The host refused the connection." ) );
                return std::nullopt;
            }
            if( std::find( types.begin(), types.end(), m.type() ) != types.end() ) {
                return ev.data;
            }
        }
        std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
    }
    err = _( "The host did not answer in time." );
    return std::nullopt;
}

bool connect_and_probe( welcome_info &info, std::string &err )
{
    {
        static_popup notice;
        notice.message( _( "Connecting to %s:%d…" ), S.address, S.port );
        ui_manager::redraw();
        refresh_display();
        if( !net::client_connect( S.address, S.port, connect_timeout_ms, err ) ) {
            return false;
        }
    }
    net::client_send( "{\"t\":\"probe\",\"ver\":" + json_quote( protocol_version_string() ) +
                      ",\"pw\":" + json_quote( S.password ) + "}" );
    const std::optional<std::string> reply = wait_for_message( { "welcome" }, handshake_timeout_ms,
            string_format( _( "Connected to %s:%d, waiting for the host…" ), S.address, S.port ), err );
    if( !reply ) {
        net::client_disconnect();
        return false;
    }
    message m( *reply );
    JsonObject jo = m.object();
    info.world = jo.get_string( "world", "" );
    info.host = jo.get_string( "host", "" );
    info.seed = static_cast<unsigned int>( std::strtoul( jo.get_string( "seed", "0" ).c_str(),
                                           nullptr, 10 ) );
    jo.read( "mods", info.mods );
    jo.read( "players", info.players );
    if( jo.has_object( "options" ) ) {
        for( const JsonMember member : jo.get_object( "options" ) ) {
            info.options[member.name()] = member.get_string();
        }
    }
    return true;
}

bool prepare_scratch_world( const welcome_info &info )
{
    std::vector<mod_id> mods;
    std::string missing;
    for( const std::string &m : info.mods ) {
        const mod_id id( m );
        if( !id.is_valid() ) {
            missing += "\n  " + m;
            continue;
        }
        mods.push_back( id );
    }
    if( !missing.empty() ) {
        popup( _( "The host plays with mods you don't have:%s\n\nInstall the same mods to join." ),
               missing );
        return false;
    }
    if( scratch_world_exists() ) {
        world_generator->delete_world( scratch_world_name, true );
    }
    WORLDINFO *world = world_generator->make_new_world_named( scratch_world_name, mods,
    [&info]( WORLDINFO & w ) {
        for( const auto &[name, value] : info.options ) {
            const auto it = w.WORLD_OPTIONS.find( name );
            if( it != w.WORLD_OPTIONS.end() ) {
                it->second.setValue( value );
            }
        }
    } );
    if( world == nullptr ) {
        popup( _( "Could not create the co-op client world." ) );
        return false;
    }
    world_generator->set_active_world( world );
    try {
        g->setup();
    } catch( const std::exception &e ) {
        popup( _( "Could not load the game data for the host's mods:\n%s" ), e.what() );
        return false;
    }
    return true;
}

void abort_join( const std::string &why )
{
    net::client_disconnect();
    net::clear_events();
    set_role( role::none );
    get_avatar() = avatar();
    world_generator->set_active_world( nullptr );
    if( scratch_world_exists() ) {
        world_generator->delete_world( scratch_world_name, true );
    }
    if( !why.empty() ) {
        popup( "%s", why );
    }
    log( "join aborted: " + why );
}

void remember_address()
{
    session_settings &s = settings();
    const std::string full = S.port == default_port ? S.address : string_format( "%s:%d", S.address,
                             S.port );
    s.last_address = full;
    auto &recent = s.recent_addresses;
    recent.erase( std::remove( recent.begin(), recent.end(), full ), recent.end() );
    recent.insert( recent.begin(), full );
    if( recent.size() > 8 ) {
        recent.resize( 8 );
    }
    save_settings();
}

// ---- applying host state ----------------------------------------------------

// Tilesets pick a monster's sprite variant from a per-monster seed, which is
// the monster's address.  Ours differ from the host's, so draw our copies with
// the host's address (the key the host sends them under) to look the same.
std::unordered_map<const monster *, uintptr_t> monster_seeds;

void rebuild_monster_seeds()
{
    monster_seeds.clear();
    for( const auto &[key, mon] : S.monsters ) {
        if( mon ) {
            monster_seeds[mon.get()] = static_cast<uintptr_t>( std::strtoull( key.c_str(), nullptr, 10 ) );
        }
    }
}

// Takes one of our monster copies off the map.  The game may already have
// dropped it on its own (e.g. when the map scrolled while riding a vehicle),
// so only remove what it still tracks.
void remove_monster_copy( const monster &mon )
{
    for( const monster &tracked : g->all_monsters() ) {
        if( &tracked == &mon ) {
            g->remove_zombie( mon );
            return;
        }
    }
}

void remove_all_monsters()
{
    for( auto &[key, mon] : S.monsters ) {
        if( mon ) {
            remove_monster_copy( *mon );
        }
    }
    S.monsters.clear();
    g->clear_zombies();
    rebuild_monster_seeds();
}

void remove_npc( int id )
{
    const character_id cid( id );
    for( npc &guy : g->all_npcs() ) {
        if( guy.getID() == cid ) {
            g->erase_npc( cid );
            break;
        }
    }
    S.npc_ids.erase( id );
}

void remove_all_npcs()
{
    std::vector<character_id> ids;
    for( npc &guy : g->all_npcs() ) {
        ids.push_back( guy.getID() );
    }
    for( const character_id &id : ids ) {
        g->erase_npc( id );
    }
    S.npc_ids.clear();
}

void apply_submaps( JsonIn &jsin )
{
    get_map().mp_apply_submaps( jsin );
}

// Keeps the local map centered on our character, as the rest of the game
// expects.  A long jump (joining, or being pulled along by the host) reloads
// the map in one go instead of scrolling it submap by submap.
void recenter_map()
{
    avatar &u = get_avatar();
    map &here = get_map();
    const tripoint_abs_sm want = reality_bubble_origin_from_player( u.abs_pos(),
                                 g_reality_bubble_size );
    const point_rel_sm delta = want.xy() - here.get_abs_sub();
    if( delta == point_rel_sm::zero() ) {
        return;
    }
    if( std::abs( delta.x() ) > 2 || std::abs( delta.y() ) > 2 ) {
        // A long jump (e.g. pulled along to the host) reloads the whole map,
        // which drops creatures our copies still refer to.  Take them off
        // first and have the host send everything again.
        remove_all_monsters();
        remove_all_npcs();
        g->load_map( want.xy() );
        net::client_send( "{\"t\":\"resync\"}" );
    } else {
        g->update_map( u );
    }
}

void apply_state( message &m )
{
    JsonObject jo = m.object();
    avatar &u = get_avatar();
    map &here = get_map();
    const int old_z = u.abs_pos().z();

    if( jo.has_int( "turn" ) ) {
        const time_point turn = time_point::from_turn( jo.get_int( "turn" ) );
        if( turn != calendar::turn ) {
            calendar::turn = turn;
        }
    }
    if( jo.has_bool( "busy" ) ) {
        S.busy = jo.get_bool( "busy" );
    }
    // Weather is the host's, not rolled locally.
    if( jo.has_array( "weather" ) ) {
        JsonArray w = jo.get_array( "weather" );
        weather_manager &wm = get_weather();
        const weather_type_id id( w.get_string( 0 ) );
        if( id.is_valid() ) {
            wm.weather_id = id;
        }
        wm.temperature = units::from_millidegree_celsius( w.get_int( 1 ) );
        wm.windspeed = w.get_int( 2 );
        wm.winddirection = w.get_int( 3 );
        // Keep the local weather generator from overriding it.
        wm.nextweather = calendar::turn + 1_days;
    }

    // Our own character first: the local map is always centered on it.
    if( jo.has_member( "you" ) ) {
        try {
            u.mp_mirror_load( *jo.get_raw( "you" ) );
            u.controlling_vehicle = jo.get_bool( "driving", false );
        } catch( const std::exception &e ) {
            log( std::string( "mirror failed: " ) + e.what() );
        }
        here.invalidate_visibility_caches();
    }
    recenter_map();
    const int new_z = u.abs_pos().z();
    if( new_z != old_z ) {
        mp_client_game_access::vertical_shift( *g, old_z, new_z );
    }

    if( jo.has_member( "sm" ) ) {
        try {
            apply_submaps( *jo.get_raw( "sm" ) );
        } catch( const std::exception &e ) {
            log( std::string( "submap apply failed: " ) + e.what() );
        }
    }

    if( jo.has_array( "omt" ) ) {
        overmapbuffer &omb = get_overmapbuffer( u.get_dimension() );
        for( JsonArray e : jo.get_array( "omt" ) ) {
            const tripoint_abs_omt p( e.get_int( 0 ), e.get_int( 1 ), e.get_int( 2 ) );
            const oter_str_id id( e.get_string( 3 ) );
            if( id.is_valid() ) {
                omb.ter_set( p, id.id() );
            }
        }
    }

    // ---- monsters
    if( jo.has_bool( "mon_reset" ) && jo.get_bool( "mon_reset" ) ) {
        remove_all_monsters();
    }
    if( jo.has_array( "mon_del" ) ) {
        std::vector<std::string> dels;
        jo.read( "mon_del", dels );
        for( const std::string &key : dels ) {
            const auto it = S.monsters.find( key );
            if( it != S.monsters.end() ) {
                if( it->second ) {
                    remove_monster_copy( *it->second );
                }
                S.monsters.erase( it );
            }
        }
    }
    if( jo.has_array( "mon_keys" ) && jo.has_member( "mon" ) ) {
        std::vector<std::string> keys;
        jo.read( "mon_keys", keys );
        std::vector<std::pair<std::string, shared_ptr_fast<monster>>> incoming;
        JsonIn &ji = *jo.get_raw( "mon" );
        ji.start_array();
        while( !ji.end_array() ) {
            shared_ptr_fast<monster> mon = make_shared_fast<monster>();
            mon->deserialize( ji );
            if( incoming.size() < keys.size() ) {
                incoming.emplace_back( keys[incoming.size()], mon );
            }
        }
        // Take every updated monster off the map before placing any of them:
        // within one update monsters swap places or follow each other, and
        // placing them one by one would land some on a not yet moved one.
        for( const auto &[key, mon] : incoming ) {
            const auto old = S.monsters.find( key );
            if( old != S.monsters.end() ) {
                if( old->second ) {
                    remove_monster_copy( *old->second );
                }
                S.monsters.erase( old );
            }
        }
        for( const auto &[key, mon] : incoming ) {
            const tripoint_bub_ms pos = abs_to_bub( mon->abs_pos() );
            if( !here.inbounds( pos ) ) {
                continue;
            }
            // The host is the authority on who stands where: whatever still
            // occupies the tile here is stale.
            if( monster *const stale = g->critter_at<monster>( pos, true ) ) {
                for( auto it = S.monsters.begin(); it != S.monsters.end(); ++it ) {
                    if( it->second.get() == stale ) {
                        S.monsters.erase( it );
                        break;
                    }
                }
                remove_monster_copy( *stale );
            }
            if( g->place_critter_around( mon, pos, 0, true ) != nullptr ) {
                S.monsters[key] = mon;
            }
        }
    }
    rebuild_monster_seeds();

    // ---- other characters
    if( jo.has_int( "host_id" ) ) {
        S.host_id = jo.get_int( "host_id" );
    }
    if( jo.has_bool( "npc_reset" ) && jo.get_bool( "npc_reset" ) ) {
        remove_all_npcs();
    }
    if( jo.has_array( "npc_del" ) ) {
        std::vector<int> dels;
        jo.read( "npc_del", dels );
        for( const int id : dels ) {
            remove_npc( id );
        }
    }
    if( jo.has_array( "npc_keys" ) && jo.has_member( "npc" ) ) {
        std::vector<int> keys;
        jo.read( "npc_keys", keys );
        // Take the old copies away before reading the new ones: two live
        // characters with the same id confuse the game's references (and
        // two that traded places would land on each other).
        for( const int key : keys ) {
            remove_npc( key );
        }
        std::vector<std::pair<int, shared_ptr_fast<npc>>> incoming;
        JsonIn &ji = *jo.get_raw( "npc" );
        ji.start_array();
        while( !ji.end_array() ) {
            shared_ptr_fast<npc> guy = make_shared_fast<npc>();
            guy->deserialize( ji );
            if( incoming.size() < keys.size() ) {
                incoming.emplace_back( keys[incoming.size()], guy );
            }
        }
        for( const auto &[key, guy] : incoming ) {
            guy->setID( character_id( key ), true );
            guy->set_dimension( u.get_dimension() );
            if( key == S.host_id ) {
                guy->set_attitude( NPCATT_FOLLOW );
            }
            const tripoint_bub_ms pos = abs_to_bub( guy->abs_pos() );
            if( !here.inbounds( pos ) ) {
                continue;
            }
            if( const npc *stale = g->critter_at<npc>( pos, true ) ) {
                remove_npc( stale->getID().get_value() );
            }
            mp_client_game_access::add_active_npc( *g, guy );
            S.npc_ids.insert( key );
        }
    }

    // ---- messages
    if( jo.has_array( "msg" ) ) {
        for( JsonArray e : jo.get_array( "msg" ) ) {
            const int type = e.get_int( 0 );
            const std::string text = e.get_string( 1 );
            add_msg( game_message_params( static_cast<game_message_type>( type ) ), text );
        }
    }

    // The client has no turn loop, which is what normally rebuilds the light
    // and vision caches.
    mp_client_game_access::reset_light_level( *g );
    here.invalidate_visibility_caches();
    here.build_map_cache( u.abs_pos().z() );
    S.got_state = true;
    g->invalidate_main_ui_adaptor();
}

// ---- sending actions --------------------------------------------------------

void send_action( const std::string &name, const std::string &extra = std::string() )
{
    ++S.seq;
    send( string_format( "{\"t\":\"act\",\"seq\":%d,\"a\":%s%s}", S.seq, json_quote( name ), extra ) );
    S.my_turn = false;
    S.awaiting_ack = true;
}

std::string pos_fields( const tripoint_bub_ms &p )
{
    const tripoint_abs_ms abs = bub_to_abs( p );
    return string_format( ",\"x\":%d,\"y\":%d,\"z\":%d", abs.x(), abs.y(), abs.z() );
}

std::string item_fields( item *it )
{
    avatar &u = get_avatar();
    return string_format( ",\"idx\":%d,\"type\":%s,\"ord\":%d,\"name\":%s", item_index_of( u, it ),
                          json_quote( it->typeId().str() ), item_type_ordinal( u, it ),
                          json_quote( item_match_name( *it ) ) );
}

bool choose_wait_duration( int &turns )
{
    uilist menu;
    menu.text = _( "Wait for how long?  (Press the pause key again to stop waiting early.)" );
    const std::vector<std::pair<std::string, time_duration>> options = {
        { _( "5 minutes" ), 5_minutes },
        { _( "30 minutes" ), 30_minutes },
        { _( "1 hour" ), 1_hours },
        { _( "2 hours" ), 2_hours },
        { _( "3 hours" ), 3_hours },
        { _( "6 hours" ), 6_hours },
    };
    for( size_t i = 0; i < options.size(); ++i ) {
        menu.addentry( static_cast<int>( i ), true, static_cast<int>( '1' + i ), options[i].first );
    }
    menu.query();
    if( menu.ret < 0 || static_cast<size_t>( menu.ret ) >= options.size() ) {
        return false;
    }
    turns = to_turns<int>( options[menu.ret].second );
    return true;
}

// Opens the pickup menu for a tile and returns the chosen items in the
// host's [[index, count, type, ordinal], ...] form ("" if nothing chosen).
// Filled by client_capture_pickup() while choose_pickup runs the pickup UI.
bool capturing_pickup = false;
std::optional<std::vector<pickup::pick_drop_selection>> pickup_capture;

std::string choose_pickup( const tripoint_bub_ms &pos )
{
    avatar &u = get_avatar();
    map &here = get_map();
    if( here.i_at( pos ).empty() ) {
        add_msg( m_info, _( "There is nothing to pick up there." ) );
        return std::string();
    }
    // The usual side panel of single player; when it is done, the game
    // would start picking up, which hands the choice to us instead.
    pickup_capture.reset();
    capturing_pickup = true;
    pickup::pick_up( pos, 0 );
    capturing_pickup = false;
    if( !pickup_capture || pickup_capture->empty() ) {
        pickup_capture.reset();
        return std::string();
    }
    const std::vector<pickup::pick_drop_selection> picked = std::move( *pickup_capture );
    pickup_capture.reset();
    ( void )u;
    std::vector<item *> ground;
    for( item *it : here.i_at( pos ) ) {
        ground.push_back( it );
    }
    // A vehicle's cargo space there is offered too.
    std::vector<item *> cargo;
    if( const optional_vpart_position vp = here.veh_at( pos ) ) {
        const int part = vp->vehicle().part_with_feature( vp->part_index(), "CARGO", false );
        if( part >= 0 ) {
            for( item *it : vp->vehicle().get_items( part ) ) {
                cargo.push_back( it );
            }
        }
    }
    std::string list;
    for( const pickup::pick_drop_selection &sel : picked ) {
        const item *target = sel.target.get();
        bool in_cargo = false;
        auto found = std::find( ground.begin(), ground.end(), target );
        std::vector<item *> *pile = &ground;
        if( found == ground.end() ) {
            found = std::find( cargo.begin(), cargo.end(), target );
            if( found == cargo.end() ) {
                continue;
            }
            in_cargo = true;
            pile = &cargo;
        }
        const int index = static_cast<int>( found - pile->begin() );
        const int ordinal = static_cast<int>( std::count_if( pile->begin(), found,
        [target]( const item * g ) {
            return g->typeId() == target->typeId();
        } ) );
        list += list.empty() ? "" : ",";
        list += string_format( "[%d,%d,%s,%d%s]", index, sel.quantity.value_or( 0 ),
                               json_quote( target->typeId().str() ), ordinal, in_cargo ? ",1" : "" );
    }
    if( list.empty() ) {
        add_msg( m_info, _( "Those items can't be picked up in co-op yet." ) );
    }
    return list;
}

void request_pickup( const tripoint_bub_ms &pos )
{
    const std::string list = choose_pickup( pos );
    if( !list.empty() ) {
        send_action( "pickup", pos_fields( pos ) + ",\"items\":[" + list + "]" );
    }
}

void request_drop( const std::optional<tripoint_bub_ms> &where = std::nullopt )
{
    avatar &u = get_avatar();
    const drop_locations locs = game_menus::inv::multidrop( u );
    if( locs.empty() ) {
        return;
    }
    std::string list;
    for( const drop_location &loc : locs ) {
        item *it = loc.loc.get();
        if( it == nullptr ) {
            continue;
        }
        list += list.empty() ? "" : ",";
        list += string_format( "[%d,%d,%s,%d,%s]", item_index_of( u, it ), loc.count,
                               json_quote( it->typeId().str() ), item_type_ordinal( u, it ),
                               json_quote( item_match_name( *it ) ) );
    }
    if( !list.empty() ) {
        send_action( "drop", ( where ? pos_fields( *where ) : std::string() ) + ",\"items\":[" + list + "]" );
    }
}

// The recipe is picked from what our copy of the character can make; the
// host checks it again and does the crafting.
void request_craft( const bool is_long, const bool again )
{
    avatar &u = get_avatar();
    static recipe_id last_recipe;
    static int last_batch = 1;
    recipe_id id = last_recipe;
    int batch = last_batch;
    if( !again || last_recipe.is_empty() ) {
        const recipe *picked = select_crafting_recipe( batch, u );
        if( picked == nullptr ) {
            return;
        }
        id = picked->ident();
        last_recipe = id;
        last_batch = batch;
    }
    send_action( "craft", string_format( ",\"recipe\":%s,\"batch\":%d,\"long\":%s",
                                         json_quote( id.str() ), std::max( 1, batch ), is_long ? "true" : "false" ) );
}

// Aiming and firing the wielded gun.  The usual targeting UI runs here on
// our copy of the character, with time to spare so a whole aim fits in one
// go; the host then fires with the aim we reached and takes the time spent.
void request_fire()
{
    avatar &u = get_avatar();
    // A reach weapon (spear, whip...) attacks at a distance instead.
    if( u.is_armed() && !( u.primary_weapon().is_gun() &&
                           !u.primary_weapon().gun_current_mode().melee() ) &&
        u.primary_weapon().reach_range( u ) > 1 ) {
        const target_handler::trajectory traj = target_handler::mode_reach( u, u.primary_weapon() );
        if( !traj.empty() ) {
            send_action( "reach", pos_fields( traj.back() ) );
        }
        return;
    }
    if( !u.is_armed() || !u.primary_weapon().is_gun() ) {
        add_msg( m_info, _( "You are not wielding a gun." ) );
        return;
    }
    if( u.primary_weapon().is_gunmod() ) {
        add_msg( m_info, _( "The %s must be attached to a gun, it can not be fired separately." ),
                 u.primary_weapon().tname() );
        return;
    }
    constexpr int budget = 100000;
    std::unique_ptr<aim_activity_actor> aim = aim_activity_actor::use_wielded();
    target_handler::trajectory trajectory;
    int spent = 0;
    // An empty result without abort means "aimed as well as possible and
    // waited": keep going, as the next turn would.
    for( int round = 0; round < 8 && trajectory.empty() && !aim->aborted; ++round ) {
        u.set_moves( budget );
        trajectory = target_handler::mode_fire( u, *aim );
        spent += budget - std::max( 0, u.get_moves() );
    }
    if( aim->aborted ) {
        if( aim->reload_requested ) {
            send_action( "reload" );
        }
        return;
    }
    if( trajectory.empty() ) {
        return;
    }
    const tripoint_bub_ms target = trajectory.back();
    if( target == u.bub_pos() ) {
        return;
    }
    send_action( "fire", pos_fields( target ) +
                 string_format( ",\"recoil\":%.3f,\"aim_moves\":%d,\"mode\":%s", u.recoil, spent,
                                json_quote( u.primary_weapon().gun_get_mode_id().str() ) ) );
}

// Addresses an item lying next to us for the host: tile, index in its pile
// and which one of its type it is.  nullopt if it is not within reach.
std::optional<std::string> ground_item_fields( const item *it )
{
    avatar &u = get_avatar();
    map &here = get_map();
    for( const tripoint_bub_ms &p : here.points_in_radius( u.bub_pos(), 1 ) ) {
        std::vector<item *> ground;
        for( item *g : here.i_at( p ) ) {
            ground.push_back( g );
        }
        const auto found = std::find( ground.begin(), ground.end(), it );
        if( found == ground.end() ) {
            continue;
        }
        const int ordinal = static_cast<int>( std::count_if( ground.begin(), found,
        [it]( const item * g ) {
            return g->typeId() == it->typeId();
        } ) );
        return pos_fields( p ) + string_format( ",\"gidx\":%d,\"type\":%s,\"ord\":%d",
                                                static_cast<int>( found - ground.begin() ), json_quote( it->typeId().str() ), ordinal );
    }
    return std::nullopt;
}

// Fields addressing an item for the host, carried or next to us.
std::optional<std::string> any_item_fields( item *it )
{
    if( item_index_of( get_avatar(), it ) >= 0 ) {
        return item_fields( it );
    }
    return ground_item_fields( it );
}

// Throwing: pick the item and aim with the usual targeting UI here; the host
// throws it.  The target goes as tx/ty/tz (x/y/z address the item).
void request_throw()
{
    avatar &u = get_avatar();
    item *it = game_menus::inv::titled_menu( u, _( "Throw item" ),
               _( "You don't have any items to throw." ) );
    if( it == nullptr ) {
        return;
    }
    if( item_index_of( u, it ) < 0 ) {
        add_msg( m_info, _( "You don't have that item." ) );
        return;
    }
    const int range = u.throw_range( *it );
    if( range == 0 ) {
        add_msg( m_info, _( "That is too heavy to throw." ) );
        return;
    }
    const target_handler::trajectory traj = target_handler::mode_throw( u, *it, false );
    if( traj.empty() ) {
        return;
    }
    const tripoint_abs_ms abs = bub_to_abs( traj.back() );
    send_action( "throw", item_fields( it ) + string_format( ",\"tx\":%d,\"ty\":%d,\"tz\":%d",
                 abs.x(), abs.y(), abs.z() ) );
}

// The wield menu also offers items lying next to the character.
void request_ground_wield( const item *it )
{
    avatar &u = get_avatar();
    map &here = get_map();
    for( const tripoint_bub_ms &p : here.points_in_radius( u.bub_pos(), 1 ) ) {
        std::vector<item *> ground;
        for( item *g : here.i_at( p ) ) {
            ground.push_back( g );
        }
        const auto found = std::find( ground.begin(), ground.end(), it );
        if( found == ground.end() ) {
            continue;
        }
        const int ordinal = static_cast<int>( std::count_if( ground.begin(), found,
        [it]( const item * g ) {
            return g->typeId() == it->typeId();
        } ) );
        send_action( "wield", pos_fields( p ) + string_format( ",\"gidx\":%d,\"type\":%s,\"ord\":%d",
                     static_cast<int>( found - ground.begin() ), json_quote( it->typeId().str() ), ordinal ) );
        return;
    }
    add_msg( m_info, _( "You can't reach that." ) );
}

void request_item_action( const std::string &name, item *it )
{
    if( it == nullptr ) {
        return;
    }
    send_action( name, item_fields( it ) );
}

void request_use( item *it );

// Inventory: look at an item and pick what to do with it.  The host does it.
void view_inventory()
{
    avatar &u = get_avatar();
    item *it = game_menus::inv::titled_menu( u, _( "Inventory" ) );
    if( it == nullptr ) {
        return;
    }
    enum { act_info, act_wield, act_wear, act_takeoff, act_eat, act_drop, act_use, act_read, act_disassemble };
    uilist menu;
    menu.text = it->display_name();
    menu.addentry( act_info, true, 'i', _( "Examine" ) );
    menu.addentry( act_wield, !u.is_worn( *it ), 'w',
                   u.is_wielding( *it ) ? _( "Put away" ) : _( "Wield" ) );
    if( u.is_worn( *it ) ) {
        menu.addentry( act_takeoff, true, 'T', _( "Take off" ) );
    } else if( it->is_armor() ) {
        menu.addentry( act_wear, true, 'W', _( "Wear" ) );
    }
    if( it->is_comestible() ) {
        menu.addentry( act_eat, true, 'E', _( "Eat / drink / use" ) );
    }
    if( it->type->has_use() ) {
        menu.addentry( act_use, true, 'a', _( "Use" ) );
    }
    if( it->is_book() ) {
        menu.addentry( act_read, true, 'R', _( "Read" ) );
    }
    menu.addentry( act_disassemble, true, 'D', _( "Disassemble" ) );
    menu.addentry( act_drop, true, 'd', _( "Drop" ) );
    menu.query();
    if( menu.ret == act_info ) {
        popup( "%s\n\n%s", it->display_name(), it->info_string() );
        return;
    }
    if( menu.ret < 0 ) {
        return;
    }
    if( !S.my_turn || S.awaiting_ack ) {
        // The waiting notice at the top already says whose turn it is.
        return;
    }
    switch( menu.ret ) {
        case act_wield:
            if( u.is_wielding( *it ) ) {
                send_action( "unwield" );
            } else {
                request_item_action( "wield", it );
            }
            break;
        case act_wear:
            request_item_action( "wear", it );
            break;
        case act_takeoff:
            request_item_action( "takeoff", it );
            break;
        case act_eat:
            request_item_action( "eat", it );
            break;
        case act_use:
            request_use( it );
            break;
        case act_read:
            send_action( "read", item_fields( it ) );
            break;
        case act_disassemble:
            send_action( "disassemble", item_fields( it ) );
            break;
        case act_drop:
            send_action( "drop", string_format( ",\"items\":[[%d,0,%s,%d,%s]]", item_index_of( u, it ),
                                                json_quote( it->typeId().str() ), item_type_ordinal( u, it ),
                                                json_quote( item_match_name( *it ) ) ) );
            break;
        default:
            break;
    }
}

// "Use" (a): pick the item, and the use method when it has several (like
// avatar::invoke_item does); the host performs it.
void request_use( item *it )
{
    avatar &u = get_avatar();
    if( it == nullptr ) {
        it = game_menus::inv::use( u );
        if( it == nullptr ) {
            return;
        }
    }
    const std::optional<std::string> where = any_item_fields( it );
    if( !where ) {
        add_msg( m_info, _( "You can't reach that." ) );
        return;
    }
    std::string method;
    const auto &methods = it->type->use_methods;
    if( it->type->has_use() && methods.size() > 1 ) {
        uilist umenu;
        umenu.text = string_format( _( "What to do with your %s?" ), it->tname() );
        umenu.hilight_disabled = true;
        for( const auto &e : methods ) {
            const auto res = e.second.can_call( u, *it, false, u.bub_pos() );
            umenu.addentry_desc( MENU_AUTOASSIGN, res.success(), MENU_AUTOASSIGN, e.second.get_name(),
                                 res.str() );
        }
        umenu.query();
        if( umenu.ret < 0 || umenu.ret >= static_cast<int>( methods.size() ) ) {
            return;
        }
        method = std::next( methods.begin(), umenu.ret )->first;
    }
    send_action( "use", *where + ",\"method\":" + json_quote( method ) );
}

// Butcher (B): corpses on our tile; the host runs the butchering activity,
// which checks tools, light and so on itself.
void request_butcher()
{
    avatar &u = get_avatar();
    map &here = get_map();
    std::vector<item *> ground;
    std::vector<int> corpses;
    for( item *it : here.i_at( u.bub_pos() ) ) {
        if( it->is_corpse() ) {
            corpses.push_back( static_cast<int>( ground.size() ) );
        }
        ground.push_back( it );
    }
    if( corpses.empty() ) {
        add_msg( m_info, _( "There are no corpses here to butcher." ) );
        return;
    }
    std::vector<int> chosen;
    if( corpses.size() == 1 ) {
        chosen = corpses;
    } else {
        uilist cmenu;
        cmenu.text = _( "Butcher what?" );
        for( size_t i = 0; i < corpses.size(); ++i ) {
            cmenu.addentry( static_cast<int>( i ), true, MENU_AUTOASSIGN, ground[corpses[i]]->display_name() );
        }
        cmenu.addentry( static_cast<int>( corpses.size() ), true, 'a', _( "All corpses" ) );
        cmenu.query();
        if( cmenu.ret < 0 ) {
            return;
        }
        if( cmenu.ret == static_cast<int>( corpses.size() ) ) {
            chosen = corpses;
        } else {
            chosen.push_back( corpses[cmenu.ret] );
        }
    }
    const std::vector<std::pair<std::string, std::string>> kinds = {
        { "ACT_BUTCHER", _( "Quick butchery" ) },
        { "ACT_BUTCHER_FULL", _( "Full butchery" ) },
        { "ACT_FIELD_DRESS", _( "Field dress corpse" ) },
        { "ACT_SKIN", _( "Skin corpse" ) },
        { "ACT_BLEED", _( "Bleed corpse" ) },
        { "ACT_QUARTER", _( "Quarter corpse" ) },
        { "ACT_DISMEMBER", _( "Dismember corpse" ) },
        { "ACT_DISSECT", _( "Dissect corpse" ) },
    };
    uilist kmenu;
    kmenu.text = _( "Choose type of butchery:" );
    for( size_t i = 0; i < kinds.size(); ++i ) {
        kmenu.addentry( static_cast<int>( i ), true, MENU_AUTOASSIGN, kinds[i].second );
    }
    kmenu.query();
    if( kmenu.ret < 0 || kmenu.ret >= static_cast<int>( kinds.size() ) ) {
        return;
    }
    std::string list;
    for( const int idx : chosen ) {
        const item *c = ground[idx];
        const int ordinal = static_cast<int>( std::count_if( ground.begin(), ground.begin() + idx,
        [c]( const item * g ) {
            return g->typeId() == c->typeId();
        } ) );
        list += list.empty() ? "" : ",";
        list += string_format( "[%d,0,%s,%d]", idx, json_quote( c->typeId().str() ), ordinal );
    }
    send_action( "butcher", ",\"kind\":" + json_quote( kinds[kmenu.ret].first ) +
                 ",\"items\":[" + list + "]" );
}

// Tab: the host attacks the most dangerous hostile in reach, or waits a turn.
void request_autoattack()
{
    send_action( "autoattack" );
}

bool is_local_ui_action( action_id act )
{
    switch( act ) {
        case ACTION_LOOK:
        case ACTION_PEEK:
        case ACTION_LIST_ITEMS:
        case ACTION_MAP:
        case ACTION_SKY:
        case ACTION_MISSIONS:
        case ACTION_SCORES:
        case ACTION_DIARY:
        case ACTION_FACTIONS:
        case ACTION_MORALE:
        case ACTION_MESSAGES:
        case ACTION_OPEN_WIKI:
        case ACTION_OPEN_HHG:
        case ACTION_HELP:
        case ACTION_OPTIONS:
        case ACTION_AUTOPICKUP:
        case ACTION_AUTONOTES:
        case ACTION_SAFEMODE:
        case ACTION_DISTRACTION_MANAGER:
        case ACTION_COLOR:
        case ACTION_WORLD_MODS:
        case ACTION_PL_INFO:
        case ACTION_KEYBINDINGS:
        case ACTION_TOGGLE_FULLSCREEN:
        case ACTION_TOGGLE_PIXEL_MINIMAP:
        case ACTION_TOGGLE_PANEL_ADM:
        case ACTION_PANEL_MGMT:
        case ACTION_RELOAD_TILESET:
        case ACTION_ZOOM_IN:
        case ACTION_ZOOM_OUT:
        case ACTION_CENTER:
        case ACTION_SHIFT_N:
        case ACTION_SHIFT_NE:
        case ACTION_SHIFT_E:
        case ACTION_SHIFT_SE:
        case ACTION_SHIFT_S:
        case ACTION_SHIFT_SW:
        case ACTION_SHIFT_W:
        case ACTION_SHIFT_NW:
        case ACTION_TOGGLE_MAP_MEMORY:
        case ACTION_TOGGLE_HOUR_TIMER:
        case ACTION_TOGGLE_ZONE_OVERLAY:
        case ACTION_DISPLAY_SUBMAP_GRID:
        case ACTION_TIMEOUT:
        case ACTION_NULL:
        case ACTION_COOP_CHAT:
        case ACTION_COOP_MENU:
        case ACTION_COOP_PLAYERS:
            return true;
        default:
            return false;
    }
}

bool try_reconnect()
{
    for( int attempt = 1; attempt <= 3; ++attempt ) {
        welcome_info info;
        std::string err;
        log( string_format( "reconnect attempt %d", attempt ) );
        if( connect_and_probe( info, err ) ) {
            net::client_send( "{\"t\":\"join\",\"name\":" + json_quote( S.my_name ) + ",\"ver\":" +
                              json_quote( protocol_version_string() ) + ",\"pw\":" + json_quote( S.password ) +
                              ",\"char\":null}" );
            if( wait_for_message( { "joined" }, handshake_timeout_ms,
                                  _( "Rejoining the session…" ), err ) ) {
                S.my_turn = false;
                S.awaiting_ack = false;
                S.waiting = false;
                return true;
            }
        }
        log( "reconnect failed: " + err );
        static_popup notice;
        notice.message( _( "Connection lost.  Retrying in a moment… (%d/3)" ), attempt );
        ui_manager::redraw();
        refresh_display();
        std::this_thread::sleep_for( std::chrono::milliseconds( 2500 ) );
    }
    return false;
}

} // namespace

// ============================================================================
// Network message handling
// ============================================================================

void send( const std::string &msg )
{
    net::client_send( msg );
}

void send_chat( const std::string &text )
{
    send( "{\"t\":\"chat\",\"text\":" + json_quote( text ) + "}" );
}

// A question raised by our own action while the host runs it; the host
// waits for the answer.  Kinds: a menu choice, a direction, a map tile, text.
void answer_question( message &m )
{
    JsonObject jo = m.object();
    const int id = jo.get_int( "id", 0 );
    const std::string kind = jo.get_string( "kind", "choice" );
    std::string reply;
    if( kind == "dir" ) {
        const std::optional<tripoint_rel_ms> dir = choose_direction( jo.get_string( "text", "" ),
                jo.get_bool( "vertical", false ) );
        reply = dir ? string_format( ",\"ok\":true,\"dx\":%d,\"dy\":%d,\"dz\":%d", dir->x(), dir->y(),
                                     dir->z() ) : ",\"ok\":false";
    } else if( kind == "peek" ) {
        g->peek( tripoint_rel_ms( jo.get_int( "dx", 0 ), jo.get_int( "dy", 0 ), jo.get_int( "dz", 0 ) ) );
    } else if( kind == "multi" ) {
        // Several items: tick them (with an amount when there are several),
        // then confirm.
        std::vector<std::string> opts;
        std::vector<int> maxes;
        jo.read( "opts", opts );
        jo.read( "max", maxes );
        std::vector<int> chosen( opts.size(), 0 );
        bool confirmed = false;
        int cursor = 0;
        while( true ) {
            uilist menu;
            menu.text = jo.get_string( "text", "" ) + "\n" +
                        _( "Select items, then choose \"Done\"." );
            for( size_t i = 0; i < opts.size(); ++i ) {
                const int max = i < maxes.size() ? maxes[i] : 1;
                const std::string mark = chosen[i] == 0 ? "[ ] " : max > 1 ?
                                         string_format( "[%d] ", chosen[i] ) : "[x] ";
                menu.addentry( static_cast<int>( i ), true, MENU_AUTOASSIGN, mark + opts[i] );
            }
            const int done = static_cast<int>( opts.size() );
            menu.addentry( done, true, 'D', _( "Done" ) );
            menu.selected = cursor;
            menu.query();
            if( menu.ret == done ) {
                confirmed = true;
                break;
            }
            if( menu.ret < 0 || menu.ret >= done ) {
                break;
            }
            cursor = menu.ret;
            const int max = menu.ret < static_cast<int>( maxes.size() ) ? maxes[menu.ret] : 1;
            if( chosen[menu.ret] > 0 ) {
                chosen[menu.ret] = 0;
            } else if( max > 1 ) {
                string_input_popup amount;
                amount.title( string_format( _( "How many (max %d)?" ), max ) )
                .text( std::to_string( max ) )
                .only_digits( true );
                const int n = amount.query_int();
                if( !amount.canceled() ) {
                    chosen[menu.ret] = std::clamp( n, 0, max );
                }
            } else {
                chosen[menu.ret] = 1;
            }
        }
        std::string picks;
        if( confirmed ) {
            for( size_t i = 0; i < chosen.size(); ++i ) {
                if( chosen[i] > 0 ) {
                    picks += picks.empty() ? "" : ",";
                    picks += string_format( "[%d,%d]", static_cast<int>( i ), chosen[i] );
                }
            }
        }
        reply = ",\"picks\":[" + picks + "]";
    } else if( kind == "target" ) {
        // Targeting inside our action (an item, a turret...): a creature in
        // range, or any spot.
        avatar &u = get_avatar();
        const int range = std::max( 1, jo.get_int( "range", 60 ) );
        std::vector<Creature *> targets = g->get_creatures_if( [&]( const Creature & c ) {
            return &c != &u && u.sees( c ) && rl_dist( u.bub_pos(), c.bub_pos() ) <= range &&
                   c.attitude_to( u ) == Attitude::A_HOSTILE;
        } );
        std::sort( targets.begin(), targets.end(), [&u]( const Creature * a, const Creature * b ) {
            return rl_dist( u.bub_pos(), a->bub_pos() ) < rl_dist( u.bub_pos(), b->bub_pos() );
        } );
        uilist menu;
        menu.text = jo.get_string( "text", "" );
        for( size_t i = 0; i < targets.size(); ++i ) {
            menu.addentry( static_cast<int>( i ), true, MENU_AUTOASSIGN, string_format( _( "%1$s (%2$d)" ),
                           targets[i]->disp_name(), rl_dist( u.bub_pos(), targets[i]->bub_pos() ) ) );
        }
        const int pick_spot = static_cast<int>( targets.size() );
        menu.addentry( pick_spot, true, 's', _( "Choose a spot…" ) );
        menu.query();
        std::optional<tripoint_bub_ms> pos;
        if( menu.ret >= 0 && menu.ret < pick_spot ) {
            pos = targets[menu.ret]->bub_pos();
        } else if( menu.ret == pick_spot ) {
            pos = g->look_around();
        }
        reply = pos ? ",\"ok\":true" + pos_fields( *pos ) : ",\"ok\":false";
    } else if( kind == "tile" ) {
        add_msg( m_info, "%s", jo.get_string( "text", "" ) );
        const std::optional<tripoint_bub_ms> pos = g->look_around();
        reply = pos ? ",\"ok\":true" + pos_fields( *pos ) : ",\"ok\":false";
    } else if( kind == "pickup" ) {
        // The host's examine of a container etc. wants us to pick items there.
        const tripoint_bub_ms pos = abs_to_bub( tripoint_abs_ms( jo.get_int( "x", 0 ),
                                                jo.get_int( "y", 0 ), jo.get_int( "z", 0 ) ) );
        reply = ",\"items\":[" + choose_pickup( pos ) + "]";
    } else if( kind == "text" ) {
        string_input_popup popup;
        popup.title( jo.get_string( "title", "" ) )
        .description( jo.get_string( "desc", "" ) )
        .text( jo.get_string( "init", "" ) )
        .only_digits( jo.get_bool( "digits", false ) );
        const int max_length = jo.get_int( "max", -1 );
        if( max_length > 0 ) {
            popup.max_length( max_length );
        }
        popup.query();
        reply = popup.canceled() ? ",\"ok\":false" :
                ",\"ok\":true,\"s\":" + json_quote( popup.text() );
    } else {
        std::vector<std::string> opts;
        std::vector<bool> en;
        jo.read( "opts", opts );
        jo.read( "en", en );
        uilist menu;
        menu.text = jo.get_string( "text", "" );
        menu.allow_cancel = jo.get_bool( "cancel", false );
        for( size_t i = 0; i < opts.size(); ++i ) {
            menu.addentry( static_cast<int>( i ), i >= en.size() || en[i], MENU_AUTOASSIGN, opts[i] );
        }
        const bool any_enabled = std::any_of( menu.entries.begin(), menu.entries.end(),
        []( const uilist_entry & e ) {
            return e.enabled;
        } );
        int pick = -1;
        if( any_enabled ) {
            do {
                menu.query();
                pick = menu.ret;
            } while( !menu.allow_cancel && ( pick < 0 || pick >= static_cast<int>( opts.size() ) ) );
        }
        reply = string_format( ",\"i\":%d", pick );
    }
    send( string_format( "{\"t\":\"answer\",\"id\":%d%s}", id, reply ) );
    g->invalidate_main_ui_adaptor();
}

void handle_line( const std::string &line )
{
    message m( line );
    if( !m.valid() ) {
        return;
    }
    const std::string &t = m.type();
    if( t == "state" ) {
        apply_state( m );
    } else if( t == "turn" ) {
        S.my_turn = true;
        S.awaiting_ack = false;
        if( S.waiting ) {
            S.waiting = false;
        }
        S.turn_wait_started = 0;
    } else if( t == "ack" ) {
        S.awaiting_ack = false;
        get_avatar().movecounter = m.object().get_int( "spent", 0 );
        S.turn_wait_started = net::now_ms();
    } else if( t == "chat" ) {
        JsonObject jo = m.object();
        add_msg( m_info, "<color_light_cyan>%s</color>", jo.get_string( "text", "" ) );
    } else if( t == "ask" ) {
        answer_question( m );
    } else if( t == "bye" ) {
        JsonObject jo = m.object();
        S.end_reason = jo.get_string( "msg", _( "The host ended the session." ) );
        S.leaving = true;
    } else if( t == "died" ) {
        S.end_reason = _( "Your character has died." );
        S.leaving = true;
    } else if( t == "error" ) {
        JsonObject jo = m.object();
        add_msg( m_bad, _( "Co-op: %s" ), jo.get_string( "msg", "" ) );
    }
}

void flush_placeholders()
{
    std::vector<tripoint_abs_sm> pending;
    {
        std::lock_guard<std::mutex> lk( placeholders_mutex );
        pending.swap( placeholders );
    }
    if( pending.empty() || !S.in_game ) {
        return;
    }
    std::string list;
    mapbuffer &mb = get_map().get_mapbuffer();
    for( const tripoint_abs_sm &p : pending ) {
        // Skip stubs that real data from the host has already replaced.
        const submap *sm = mb.lookup_submap_in_memory( p );
        const ter_id stub = p.z() > 0 ? t_open_air : t_null;
        if( sm == nullptr || !sm->is_uniform || sm->get_ter( point_sm_ms( 0, 0 ) ) != stub ) {
            continue;
        }
        list += list.empty() ? "" : ",";
        list += string_format( "[%d,%d,%d]", p.x(), p.y(), p.z() );
    }
    if( !list.empty() ) {
        send( "{\"t\":\"need\",\"sm\":[" + list + "]}" );
    }
}

void handle_closed( const std::string &reason )
{
    if( S.leaving ) {
        return;
    }
    log( "connection closed: " + reason );
    S.reconnecting = true;
    S.end_reason = string_format( _( "Lost connection to the host (%s)." ), reason );
}

std::string status_line()
{
    if( !S.in_game ) {
        return std::string();
    }
    const int rtt = net::client_rtt_ms();
    std::string turn;
    if( S.waiting ) {
        turn = colorize( _( "WAITING" ), c_light_blue );
    } else if( S.my_turn && !S.awaiting_ack ) {
        turn = colorize( _( "YOUR TURN" ), c_light_green );
    } else if( S.awaiting_ack ) {
        turn = colorize( _( "ACTING…" ), c_yellow );
    } else {
        turn = colorize( string_format( _( "%s'S TURN" ), S.host_name ), c_light_red );
    }
    return string_format( _( "%s  Co-op: connected to %s (%s:%d), ping %s" ), turn, S.host_name,
                          S.address, S.port, rtt >= 0 ? string_format( "%d ms", rtt ) : std::string( "?" ) );
}

void leave_session( bool ask )
{
    if( ask ) {
        if( !query_yn( _( "Leave the co-op session?  Your character stays in the host's world." ) ) ) {
            return;
        }
        send( "{\"t\":\"quit\"}" );
        S.end_reason.clear();
        S.leaving = true;
        return;
    }
    // Teardown after the game loop ended.
    net::client_disconnect();
    net::clear_events();
    S = state_t();
    world_generator->set_active_world( nullptr );
    if( scratch_world_exists() ) {
        world_generator->delete_world( scratch_world_name, true );
    }
}

} // namespace cata_mp::client

// ============================================================================
// Game hooks (namespace cata_mp)
// ============================================================================

namespace cata_mp
{

using client::S;

bool join_game()
{
    session_settings &s = settings();
    log_reset();
    client::S = client::state_t();

    // ---- address
    std::string text;
    {
        uilist menu;
        menu.title = _( "Join a co-op game" );
        menu.text = _( "Enter the host's address as IP or IP:port (default port 8080).\n"
                       "Same network: the host's local IPv4 address (ipconfig).  Over the internet: "
                       "the host's public address with port forwarding, or a Radmin VPN / Tailscale / "
                       "ZeroTier address." );
        menu.addentry( 0, true, 'n', _( "Enter an address…" ) );
        for( size_t i = 0; i < s.recent_addresses.size(); ++i ) {
            menu.addentry( static_cast<int>( i + 1 ), true, MENU_AUTOASSIGN, s.recent_addresses[i] );
        }
        menu.query();
        if( menu.ret < 0 ) {
            return false;
        }
        if( menu.ret == 0 ) {
            string_input_popup addr_popup;
            addr_popup.title( _( "Host address (IP or IP:port):" ) )
            .width( 40 )
            .text( s.last_address );
            addr_popup.query();
            if( addr_popup.canceled() ) {
                return false;
            }
            text = addr_popup.text();
        } else {
            text = s.recent_addresses[menu.ret - 1];
        }
    }
    if( !client::parse_address( text, S.address, S.port ) ) {
        popup( _( "\"%s\" is not a valid address." ), text );
        return false;
    }

    // ---- handshake (with a password retry)
    client::welcome_info info;
    std::string err;
    set_role( role::client );
    bool ok = client::connect_and_probe( info, err );
    if( !ok && err == _( "Wrong password." ) ) {
        string_input_popup pw_popup;
        pw_popup.title( _( "This session needs a password:" ) ).width( 30 );
        pw_popup.query();
        if( pw_popup.canceled() ) {
            client::abort_join( std::string() );
            return false;
        }
        S.password = pw_popup.text();
        ok = client::connect_and_probe( info, err );
    }
    if( !ok ) {
        client::abort_join( string_format( _( "Could not join %s:%d.\n\n%s" ), S.address, S.port, err ) );
        return false;
    }
    S.host_name = info.host;
    client::remember_address();
    log( string_format( "probe ok: world '%s', host '%s', %d mods", info.world, info.host,
                        static_cast<int>( info.mods.size() ) ) );

    // ---- character choice
    bool new_character = true;
    bool random_character = false;
    // A character template saved earlier (character creation's "save template").
    std::string template_name;
    std::vector<std::string> templates;
    for( std::string path : get_files_from_path( ".template", PATH_INFO::templatedir(), false, true ) ) {
        path.erase( path.find( ".template" ), std::string::npos );
        path.erase( 0, path.find_last_of( "\\/" ) + 1 );
        templates.push_back( path );
    }
    std::sort( templates.begin(), templates.end(), localized_compare );
    {
        uilist menu;
        menu.title = string_format( _( "Joining %s's world \"%s\"" ), info.host, info.world );
        menu.addentry( 0, true, 'n', _( "Create a new character" ) );
        menu.addentry( 1000, true, 'r', _( "Create a random character" ) );
        menu.addentry( 2000, !templates.empty(), 't', templates.empty() ?
                       _( "Load from a template (no templates saved)" ) : _( "Load from a template" ) );
        for( size_t i = 0; i < info.players.size(); ++i ) {
            menu.addentry( static_cast<int>( i + 1 ), true, MENU_AUTOASSIGN,
                           string_format( _( "Continue as %s" ), info.players[i] ) );
        }
        menu.query();
        if( menu.ret == 2000 ) {
            uilist tmenu;
            tmenu.text = _( "Which template?" );
            for( size_t i = 0; i < templates.size(); ++i ) {
                tmenu.addentry( static_cast<int>( i ), true, MENU_AUTOASSIGN, templates[i] );
            }
            tmenu.query();
            if( tmenu.ret < 0 || tmenu.ret >= static_cast<int>( templates.size() ) ) {
                client::abort_join( std::string() );
                return false;
            }
            template_name = templates[tmenu.ret];
        } else if( menu.ret == 1000 ) {
            random_character = true;
        } else if( menu.ret < 0 ) {
            client::abort_join( std::string() );
            return false;
        }
        if( menu.ret > 0 && menu.ret != 1000 && menu.ret != 2000 ) {
            new_character = false;
            S.my_name = info.players[menu.ret - 1];
        }
    }

    // ---- local scratch world with the host's mods
    if( !client::prepare_scratch_world( info ) ) {
        client::abort_join( std::string() );
        return false;
    }
    avatar &pc = get_avatar();
    pc = avatar();
    if( new_character ) {
        const character_type kind = !template_name.empty() ? character_type::TEMPLATE :
                                    random_character ? character_type::FULL_RANDOM : character_type::CUSTOM;
        if( !pc.create( kind, template_name ) ) {
            client::abort_join( std::string() );
            return false;
        }
        S.my_name = pc.get_name();
    } else {
        if( !pc.create( character_type::NOW ) ) {
            client::abort_join( std::string() );
            return false;
        }
    }

    // ---- join
    std::string join = "{\"t\":\"join\",\"name\":" + json_quote( S.my_name ) + ",\"ver\":" +
                       json_quote( protocol_version_string() ) + ",\"pw\":" + json_quote( S.password );
    if( new_character ) {
        join += ",\"char\":" + write_json( [&pc]( JsonOut & jsout ) {
            pc.serialize( jsout );
        } );
    } else {
        join += ",\"char\":null";
    }
    join += "}";
    net::client_send( join );
    if( !client::wait_for_message( { "joined" }, client::handshake_timeout_ms, _( "Joining the game…" ),
                                   err ) ) {
        client::abort_join( string_format( _( "The host did not accept you:\n\n%s" ), err ) );
        return false;
    }

    // ---- start a local game shell; the host's state replaces its contents.
    // Nothing of the local start matters, so make it one that always succeeds
    // on blank placeholder terrain.
    pc.starting_vehicle = vproto_id::NULL_ID();
    pc.random_start_location = false;
    pc.start_location = start_location_id( "sloc_shelter" );
    if( !mp_client_game_access::start_game( *g ) ) {
        client::abort_join( _( "Could not start the client game." ) );
        return false;
    }
    mp_client_game_access::set_seed( *g, info.seed );
    client::remove_all_npcs();
    client::remove_all_monsters();
    S.in_game = true;
    add_msg( m_good, _( "Co-op: joined %s's game as %s." ), S.host_name, S.my_name );
    add_msg( m_info, _( "Co-op: %s opens the co-op menu, %s chats.  Your actions are performed in turn with the host." ),
             press_x( ACTION_COOP_MENU ), press_x( ACTION_COOP_CHAT ) );
    log( "joined as " + S.my_name );
    return true;
}

bool client_do_turn()
{
    if( S.reconnecting && !S.leaving ) {
        S.reconnecting = false;
        if( !client::try_reconnect() ) {
            S.leaving = true;
        }
    }
    if( S.leaving ) {
        if( !S.end_reason.empty() ) {
            popup( "%s", S.end_reason );
        }
        // Drop our copies of the host's creatures before the game tears the
        // world down; held references would otherwise outlive it.
        client::remove_all_monsters();
        client::remove_all_npcs();
        g->uquit = QUIT_NOSAVED;
        return true;
    }
    pump();
    if( S.leaving || S.reconnecting ) {
        return false;
    }
    if( !S.got_state ) {
        static_popup notice;
        notice.message( "%s", _( "Receiving the world from the host…" ) );
        ui_manager::redraw();
        refresh_display();
        inp_mngr.pump_events();
        std::this_thread::sleep_for( std::chrono::milliseconds( 30 ) );
        return false;
    }
    avatar &u = get_avatar();
    // While the host acts, show it at the top of the screen like the host's
    // own "Waiting for…" notice, instead of filling the message log.
    std::unique_ptr<static_popup> waiting_notice;
    if( ( !S.my_turn || S.awaiting_ack ) && !S.waiting ) {
        waiting_notice = std::make_unique<static_popup>();
        waiting_notice->on_top( true );
        const int secs = S.turn_wait_started > 0 ?
                         static_cast<int>( ( net::now_ms() - S.turn_wait_started ) / 1000 ) : 0;
        waiting_notice->message( _( "Waiting for %s to act… (%d s)" ), S.host_name, secs );
    }
    // handle_action() expects a character that can act; the host owns the
    // real move budget.  Activities run on the host only: drop any a local
    // menu left behind.
    u.moves = 100;
    if( u.activity && *u.activity ) {
        u.activity = std::make_unique<player_activity>();
    }
    mp_client_game_access::handle_action( *g );
    return false;
}

bool client_intercept_action( action_id act,
                              const std::optional<tripoint_bub_ms> &mouse_target )
{
    if( !is_client() ) {
        return false;
    }
    if( client::is_local_ui_action( act ) ) {
        return false;
    }
    avatar &u = get_avatar();
    switch( act ) {
        case ACTION_SAVE:
            client::leave_session( true );
            return true;
        case ACTION_QUICKSAVE:
            add_msg( m_info, _( "Only the host can save the co-op world." ) );
            return true;
        case ACTION_QUICKLOAD:
        case ACTION_SUICIDE:
        case ACTION_DEBUG:
        case ACTION_LUA_CONSOLE:
        case ACTION_LUA_RELOAD:
        case ACTION_TOGGLE_DEBUG_MODE:
            add_msg( m_info, _( "That is not available to a co-op client." ) );
            return true;
        case ACTION_INVENTORY:
        case ACTION_COMPARE:
            client::view_inventory();
            return true;
        default:
            break;
    }

    // Our character's long action (pulping, crafting...) runs on the host and
    // shows up here through the mirrored state.
    // Only the host knows; menus opened here may leave a local activity.
    const bool busy = S.busy;
    if( S.waiting || busy ) {
        if( act == ACTION_PAUSE || act == ACTION_WAIT ) {
            client::send( "{\"t\":\"stop_wait\"}" );
            add_msg( m_info, S.waiting ? _( "You decide to stop waiting." ) :
                     _( "You stop what you are doing." ) );
            // Don't wait for the host to confirm: it may be busy itself.
            S.waiting = false;
        } else {
            add_msg( m_info, S.waiting ? _( "You are waiting.  Press the pause key to stop." ) :
                     _( "You are busy.  Press the pause key to stop." ) );
        }
        return true;
    }
    if( !S.my_turn || S.awaiting_ack ) {
        // The waiting notice at the top already says whose turn it is.
        return true;
    }

    switch( act ) {
        case ACTION_MOVE_FORTH:
        case ACTION_MOVE_FORTH_RIGHT:
        case ACTION_MOVE_RIGHT:
        case ACTION_MOVE_BACK_RIGHT:
        case ACTION_MOVE_BACK:
        case ACTION_MOVE_BACK_LEFT:
        case ACTION_MOVE_LEFT:
        case ACTION_MOVE_FORTH_LEFT: {
            if( u.controlling_vehicle ) {
                // At the wheel the keys steer (x) and accelerate/brake (y).
                const point_rel_ms d = get_delta_from_movement_action( act, iso_rotate::no );
                client::send_action( "drive", string_format( ",\"dx\":%d,\"dy\":%d", d.x(), d.y() ) );
                return true;
            }
            const point_rel_ms d = get_delta_from_movement_action( act, iso_rotate::yes );
            client::send_action( "move", string_format( ",\"dx\":%d,\"dy\":%d", d.x(), d.y() ) );
            return true;
        }
        case ACTION_CONTROL_VEHICLE:
            client::send_action( "control_vehicle" );
            return true;
        case ACTION_MOVE_DOWN:
            client::send_action( "vmove", ",\"dz\":-1" );
            return true;
        case ACTION_MOVE_UP:
            client::send_action( "vmove", ",\"dz\":1" );
            return true;
        case ACTION_PAUSE:
            if( u.controlling_vehicle ) {
                // At the wheel, waiting means keeping the vehicle going.
                client::send_action( "drive", ",\"dx\":0,\"dy\":0" );
                return true;
            }
            client::send_action( "pause" );
            return true;
        case ACTION_WAIT: {
            int turns = 0;
            if( client::choose_wait_duration( turns ) ) {
                S.waiting = true;
                client::send_action( "wait", string_format( ",\"turns\":%d", turns ) );
            }
            return true;
        }
        case ACTION_OPEN: {
            const std::optional<tripoint_bub_ms> p = mouse_target ? mouse_target :
                    choose_adjacent_highlight( _( "Open where?" ),
                                               pgettext( "no door, gate, curtain, etc.", "There is nothing that can be opened nearby." ),
                                               ACTION_OPEN, false );
            if( p ) {
                client::send_action( "open", client::pos_fields( *p ) );
            }
            return true;
        }
        case ACTION_CLOSE: {
            const std::optional<tripoint_bub_ms> p = mouse_target ? mouse_target :
                    choose_adjacent_highlight( _( "Close where?" ),
                                               pgettext( "no door, gate, etc.", "There is nothing that can be closed nearby." ),
                                               ACTION_CLOSE, false );
            if( p ) {
                client::send_action( "close", client::pos_fields( *p ) );
            }
            return true;
        }
        case ACTION_SMASH: {
            const std::optional<tripoint_bub_ms> p = choose_adjacent( _( "Smash where?" ), true );
            if( p ) {
                bool acid = false;
                for( const item *it : get_map().i_at( *p ) ) {
                    if( it->is_corpse() && it->damage() < it->max_damage() &&
                        ( it->get_mtype()->has_flag( MF_REVIVES ) || it->get_mtype()->zombify_into ) &&
                        it->get_mtype()->bloodType()->has_acid ) {
                        acid = query_yn( _( "Are you sure you want to pulp an acid filled corpse?" ) );
                        break;
                    }
                }
                client::send_action( "smash", client::pos_fields( *p ) +
                                     ( acid ? ",\"acid\":true" : "" ) );
            }
            return true;
        }
        case ACTION_EXAMINE: {
            std::optional<tripoint_bub_ms> p = mouse_target;
            if( !p ) {
                p = choose_adjacent( _( "Examine where?" ), true );
            }
            if( !p ) {
                return true;
            }
            map &here = get_map();
            // Another character there: the host or an NPC.  Same options
            // as the host has for us: swap, examine wounds, attack.
            if( const npc *who = g->critter_at<npc>( *p ) ) {
                const bool adjacent = rl_dist( u.bub_pos(), *p ) == 1;
                uilist menu;
                menu.text = string_format( _( "What to do with %s?" ), who->get_name() );
                menu.addentry( 0, adjacent && !who->is_enemy(), 's', _( "Swap positions" ) );
                menu.addentry( 2, true, 'w', _( "Examine wounds" ) );
                menu.addentry( 3, adjacent, 'a', _( "Attack" ) );
                menu.query();
                const tripoint_rel_ms d = *p - u.bub_pos();
                if( menu.ret == 0 ) {
                    client::send_action( "move", string_format( ",\"dx\":%d,\"dy\":%d", d.x(), d.y() ) );
                } else if( menu.ret == 2 ) {
                    const bool precise = u.get_skill_level( skill_id( "firstaid" ) ) * 4 + u.per_cur >= 20;
                    who->body_window( _( "Limbs of: " ) + who->disp_name(), true, precise, 0, 0, 0, 0.0f, 0.0f,
                                      0.0f, 0.0f, 0.0f );
                } else if( menu.ret == 3 && query_yn( _( "Really attack %s?" ), who->get_name() ) ) {
                    client::send_action( "attack", client::pos_fields( *p ) );
                }
                return true;
            }
            const bool special = here.has_flag( "CONSOLE", *p ) || here.veh_at( *p ) ||
                                 !here.tr_at( *p ).is_null() ||
                                 ( here.has_furn( *p ) ? here.furn( *p ).obj().examine != &iexamine::none :
                                   here.ter( *p ).obj().examine != &iexamine::none );
            if( special ) {
                client::send_action( "examine", client::pos_fields( *p ) );
            } else if( !here.i_at( *p ).empty() ) {
                client::request_pickup( *p );
            } else if( here.has_flag( "CONTAINER", *p ) ) {
                add_msg( _( "It is empty." ) );
            } else {
                add_msg( _( "There is nothing special about the %s." ), here.name( *p ) );
            }
            return true;
        }
        case ACTION_PICKUP:
        case ACTION_PICKUP_ALL: {
            std::optional<tripoint_bub_ms> p = mouse_target;
            if( !p ) {
                p = choose_adjacent( _( "Pick up items where?" ) );
            }
            if( p ) {
                client::request_pickup( *p );
            }
            return true;
        }
        case ACTION_PICKUP_FEET:
            client::request_pickup( u.bub_pos() );
            return true;
        case ACTION_DROP:
            client::request_drop();
            return true;
        case ACTION_DIR_DROP: {
            const std::optional<tripoint_bub_ms> p = choose_adjacent( _( "Drop where?" ) );
            if( p ) {
                client::request_drop( p );
            }
            return true;
        }
        case ACTION_WIELD: {
            item *it = game_menus::inv::wield( u );
            if( it != nullptr ) {
                if( it->is_null() ) {
                    client::send_action( "unwield" );
                } else if( item_index_of( u, it ) < 0 ) {
                    client::request_ground_wield( it );
                } else {
                    client::request_item_action( "wield", it );
                }
            }
            return true;
        }
        case ACTION_WEAR:
            client::request_item_action( "wear", game_menus::inv::wear( u ) );
            return true;
        case ACTION_FIRE:
            client::request_fire();
            return true;
        case ACTION_THROW:
            client::request_throw();
            return true;
        case ACTION_AUTOATTACK:
            client::request_autoattack();
            return true;
        case ACTION_CHAT: {
            // Only shouting for now; talking to NPCs is the host's.
            uilist menu;
            menu.text = _( "What do you want to do?" );
            menu.addentry( 0, true, 'a', _( "Yell" ) );
            menu.addentry( 1, true, 'b', _( "Yell a sentence" ) );
            menu.query();
            if( menu.ret == 0 ) {
                client::send_action( "shout" );
            } else if( menu.ret == 1 ) {
                string_input_popup popup;
                popup.title( _( "Yell a sentence" ) ).width( 64 ).max_length( 128 )
                .description( _( "Enter a sentence to yell" ) ).query();
                if( !popup.canceled() && !popup.text().empty() ) {
                    client::send_action( "shout", ",\"text\":" + json_quote( popup.text() ) );
                }
            }
            return true;
        }
        case ACTION_USE:
            client::request_use( nullptr );
            return true;
        case ACTION_BUTCHER:
            client::request_butcher();
            return true;
        case ACTION_READ: {
            item *book = game_menus::inv::read( u );
            if( book == nullptr ) {
                return true;
            }
            const std::optional<std::string> where = client::any_item_fields( book );
            if( !where ) {
                add_msg( m_info, _( "You can't reach that." ) );
                return true;
            }
            client::send_action( "read", *where );
            return true;
        }
        case ACTION_USE_WIELDED:
            if( u.is_armed() ) {
                client::request_use( &u.primary_weapon() );
            } else {
                add_msg( m_info, _( "You are not wielding anything you could use." ) );
            }
            return true;
        case ACTION_CONSTRUCT:
            // The usual menu; picking a spot sends the request to the host.
            construction_menu( false );
            return true;
        case ACTION_CRAFT:
        case ACTION_LONGCRAFT:
        case ACTION_RECRAFT:
            client::request_craft( act == ACTION_LONGCRAFT, act == ACTION_RECRAFT );
            return true;
        case ACTION_RELOAD_WEAPON:
        case ACTION_RELOAD_WIELDED:
        case ACTION_RELOAD_ITEM:
            client::send_action( "reload" );
            return true;
        case ACTION_SELECT_FIRE_MODE:
            client::send_action( "fire_mode" );
            return true;
        case ACTION_TAKE_OFF:
            client::request_item_action( "takeoff", game_menus::inv::take_off( u ) );
            return true;
        case ACTION_EAT:
        case ACTION_OPEN_CONSUME:
            client::request_item_action( "eat", game_menus::inv::consume( u ) );
            return true;
        case ACTION_TOGGLE_RUN:
            client::send_action( "move_mode", string_format( ",\"mode\":%d",
                                 u.get_movement_mode() == CMM_RUN ? CMM_WALK : CMM_RUN ) );
            return true;
        case ACTION_TOGGLE_CROUCH:
            client::send_action( "move_mode", string_format( ",\"mode\":%d",
                                 u.get_movement_mode() == CMM_CROUCH ? CMM_WALK : CMM_CROUCH ) );
            return true;
        case ACTION_TOGGLE_PRONE:
            client::send_action( "move_mode", string_format( ",\"mode\":%d",
                                 u.get_movement_mode() == CMM_PRONE ? CMM_WALK : CMM_PRONE ) );
            return true;
        case ACTION_CYCLE_MOVE:
            client::send_action( "move_mode", ",\"cycle\":true" );
            return true;
        case ACTION_OPEN_MOVEMENT: {
            uilist as_m;
            as_m.text = _( "Change to which movement mode?" );
            as_m.entries.emplace_back( CMM_RUN, true, 'r', _( "Run" ) );
            as_m.entries.emplace_back( CMM_WALK, true, 'w', _( "Walk" ) );
            as_m.entries.emplace_back( CMM_CROUCH, true, 'c', _( "Crouch" ) );
            as_m.entries.emplace_back( CMM_PRONE, true, 'p', _( "Prone" ) );
            as_m.entries.emplace_back( CMM_COUNT, true, '"', _( "Cycle move mode" ) );
            as_m.selected = 1;
            as_m.query();
            if( as_m.ret == CMM_COUNT ) {
                client::send_action( "move_mode", ",\"cycle\":true" );
            } else if( as_m.ret >= 0 && as_m.ret < CMM_COUNT ) {
                client::send_action( "move_mode", string_format( ",\"mode\":%d", as_m.ret ) );
            }
            return true;
        }
        case ACTION_JUMP: {
            if( !iexamine::can_start_jump_over_tile( u ) ) {
                return true;
            }
            const auto allowed = [&u]( const tripoint_bub_ms & pos ) {
                return iexamine::can_jump_over_tile( u, pos );
            };
            const std::optional<tripoint_bub_ms> target = choose_adjacent_highlight(
                        _( "Jump across where?" ), _( "There is no adjacent tile you can jump across." ), allowed );
            if( target ) {
                client::send_action( "jump", client::pos_fields( *target ) );
            }
            return true;
        }
        case ACTION_DISASSEMBLE: {
            item *target = game_menus::inv::disassemble( u );
            if( target == nullptr ) {
                return true;
            }
            const std::optional<std::string> where = client::any_item_fields( target );
            if( !where ) {
                add_msg( m_info, _( "You can't reach that." ) );
                return true;
            }
            client::send_action( "disassemble", *where );
            return true;
        }
        case ACTION_RESET_MOVE:
            client::send_action( "move_mode", string_format( ",\"mode\":%d", CMM_WALK ) );
            return true;
        default:
            add_msg( m_info, _( "That action is not available in co-op yet." ) );
            return true;
    }
}

} // namespace cata_mp

namespace cata_mp
{
bool input_should_return()
{
    return is_client() && ( client::S.leaving || client::S.reconnecting );
}
} // namespace cata_mp

namespace cata_mp
{
bool client_capture_pickup( std::vector<pickup::pick_drop_selection> &targets )
{
    if( !is_client() || !client::capturing_pickup ) {
        return false;
    }
    client::pickup_capture = std::move( targets );
    return true;
}

bool client_request_construct( const std::string &id, const tripoint_bub_ms &pnt )
{
    if( !is_client() ) {
        return false;
    }
    client::send_action( "construct", client::pos_fields( pnt ) + ",\"id\":" + json_quote( id ) );
    return true;
}

uintptr_t monster_sprite_seed( const monster &mon )
{
    if( is_client() ) {
        const auto it = client::monster_seeds.find( &mon );
        if( it != client::monster_seeds.end() ) {
            return it->second;
        }
    }
    return reinterpret_cast<uintptr_t>( &mon );
}

void client_placeholder_created( const tripoint_abs_sm &base )
{
    if( !is_client() ) {
        return;
    }
    std::lock_guard<std::mutex> lk( client::placeholders_mutex );
    for( const point_rel_sm &d : { point_rel_sm( 0, 0 ), point_rel_sm( 1, 0 ), point_rel_sm( 0, 1 ), point_rel_sm( 1, 1 ) } ) {
        client::placeholders.push_back( base + d );
    }
}
} // namespace cata_mp
