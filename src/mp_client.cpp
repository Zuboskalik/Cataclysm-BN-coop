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

#include "avatar.h"
#include "calendar.h"
#include "character.h"
#include "debug.h"
#include "game.h"
#include "game_constants.h"
#include "crafting_gui.h"
#include "game_inventory.h"
#include "gun_mode.h"
#include "input.h"
#include "item.h"
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
#include "pickup_token.h"
#include "player_activity.h"
#include "popup.h"
#include "recipe.h"
#include "string_formatter.h"
#include "string_input_popup.h"
#include "string_utils.h"
#include "translations.h"
#include "ui.h"
#include "ui_manager.h"
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

void remove_all_monsters()
{
    for( auto &[key, mon] : S.monsters ) {
        if( mon ) {
            g->remove_zombie( *mon );
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
        g->load_map( want.xy() );
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
                    g->remove_zombie( *it->second );
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
                    g->remove_zombie( *old->second );
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
                g->remove_zombie( *stale );
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
        // As with monsters: take all updated characters off the map first, so
        // two that traded places don't land on each other.
        for( const auto &entry : incoming ) {
            remove_npc( entry.first );
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

void request_pickup( const tripoint_bub_ms &pos )
{
    avatar &u = get_avatar();
    map &here = get_map();
    if( here.i_at( pos ).empty() ) {
        add_msg( m_info, _( "There is nothing to pick up there." ) );
        return;
    }
    const std::vector<pickup::pick_drop_selection> picked = game_menus::inv::pickup_from_tile( u,
            pos );
    if( picked.empty() ) {
        return;
    }
    std::vector<item *> ground;
    for( item *it : here.i_at( pos ) ) {
        ground.push_back( it );
    }
    std::string list;
    for( const pickup::pick_drop_selection &sel : picked ) {
        const item *target = sel.target.get();
        const auto found = std::find( ground.begin(), ground.end(), target );
        if( found == ground.end() ) {
            continue;
        }
        const int index = static_cast<int>( found - ground.begin() );
        const int ordinal = static_cast<int>( std::count_if( ground.begin(), found,
        [target]( const item * g ) {
            return g->typeId() == target->typeId();
        } ) );
        list += list.empty() ? "" : ",";
        list += string_format( "[%d,%d,%s,%d]", index, sel.quantity.value_or( 0 ),
                               json_quote( target->typeId().str() ), ordinal );
    }
    if( list.empty() ) {
        add_msg( m_info, _( "You can only pick up loose items from the ground in co-op." ) );
        return;
    }
    send_action( "pickup", pos_fields( pos ) + ",\"items\":[" + list + "]" );
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

// Picks a target for the wielded gun; the host does the shooting.
void request_fire()
{
    avatar &u = get_avatar();
    if( !u.is_armed() || !u.primary_weapon().is_gun() ) {
        add_msg( m_info, _( "You are not wielding a gun." ) );
        return;
    }
    const item &gun = u.primary_weapon();
    const int range = std::max( 1, gun.gun_range( &u ) );
    std::vector<Creature *> targets = g->get_creatures_if( [&]( const Creature & c ) {
        return &c != &u && c.is_monster() && u.sees( c ) &&
               rl_dist( u.bub_pos(), c.bub_pos() ) <= range &&
               c.attitude_to( u ) == Attitude::A_HOSTILE;
    } );
    std::sort( targets.begin(), targets.end(), [&u]( const Creature * a, const Creature * b ) {
        return rl_dist( u.bub_pos(), a->bub_pos() ) < rl_dist( u.bub_pos(), b->bub_pos() );
    } );
    uilist menu;
    menu.text = string_format( _( "Fire your %1$s (%2$s, %3$d rounds left) at:" ), gun.tname(),
                               gun.gun_current_mode().tname(), gun.ammo_remaining() );
    for( size_t i = 0; i < targets.size(); ++i ) {
        menu.addentry( static_cast<int>( i ), true, MENU_AUTOASSIGN, string_format( _( "%1$s (%2$d)" ),
                       targets[i]->disp_name(), rl_dist( u.bub_pos(), targets[i]->bub_pos() ) ) );
    }
    const int pick_spot = static_cast<int>( targets.size() );
    menu.addentry( pick_spot, true, 's', _( "Choose a spot…" ) );
    menu.query();
    std::optional<tripoint_bub_ms> target;
    if( menu.ret >= 0 && menu.ret < pick_spot ) {
        target = targets[menu.ret]->bub_pos();
    } else if( menu.ret == pick_spot ) {
        target = g->look_around();
    }
    if( !target || *target == u.bub_pos() ) {
        return;
    }
    send_action( "fire", pos_fields( *target ) );
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

void view_inventory()
{
    avatar &u = get_avatar();
    item *it = game_menus::inv::titled_menu( u, _( "Inventory (view only in co-op)" ) );
    if( it == nullptr ) {
        return;
    }
    popup( "%s\n\n%s", it->display_name(), it->info_string() );
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
        if( S.turn_wait_started > 0 && net::now_ms() - S.turn_wait_started > 3000 ) {
            add_msg( m_info, _( "Your turn." ) );
        }
        S.turn_wait_started = 0;
    } else if( t == "ack" ) {
        S.awaiting_ack = false;
        S.turn_wait_started = net::now_ms();
    } else if( t == "chat" ) {
        JsonObject jo = m.object();
        add_msg( m_info, "<color_light_cyan>%s</color>", jo.get_string( "text", "" ) );
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
    {
        uilist menu;
        menu.title = string_format( _( "Joining %s's world \"%s\"" ), info.host, info.world );
        menu.addentry( 0, true, 'n', _( "Create a new character" ) );
        menu.addentry( 1000, true, 'r', _( "Create a random character" ) );
        for( size_t i = 0; i < info.players.size(); ++i ) {
            menu.addentry( static_cast<int>( i + 1 ), true, MENU_AUTOASSIGN,
                           string_format( _( "Continue as %s" ), info.players[i] ) );
        }
        menu.query();
        if( menu.ret == 1000 ) {
            random_character = true;
        } else if( menu.ret < 0 ) {
            client::abort_join( std::string() );
            return false;
        }
        if( menu.ret > 0 && menu.ret != 1000 ) {
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
        if( !pc.create( random_character ? character_type::FULL_RANDOM : character_type::CUSTOM ) ) {
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
    // handle_action() expects a character that can act; the host owns the
    // real move budget.
    u.moves = 100;
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
    const bool busy = u.activity && *u.activity;
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
        add_msg( m_info, _( "Not your turn yet: %s is acting." ), S.host_name );
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
            const point_rel_ms d = get_delta_from_movement_action( act, iso_rotate::yes );
            client::send_action( "move", string_format( ",\"dx\":%d,\"dy\":%d", d.x(), d.y() ) );
            return true;
        }
        case ACTION_MOVE_DOWN:
            client::send_action( "vmove", ",\"dz\":-1" );
            return true;
        case ACTION_MOVE_UP:
            client::send_action( "vmove", ",\"dz\":1" );
            return true;
        case ACTION_PAUSE:
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
            client::send_action( "move_mode", string_format( ",\"mode\":%d",
                                 ( static_cast<int>( u.get_movement_mode() ) + 1 ) % CMM_COUNT ) );
            return true;
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
