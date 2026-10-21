// Co-op multiplayer: host side.
//
// Remote players are proxy NPCs.  The AI never runs for them; instead
// npcmove() calls host_proxy_turn(), which waits for the owning player's next
// action, executes it on the proxy and streams the resulting world state back.

#include "mp_session.h"

#include <algorithm>
#include <map>
#include <optional>
#include <chrono>
#include <set>
#include <thread>
#include <sstream>

#include "activity_actor_definitions.h"
#include "vehicle/vehicle_grab.h"
#include "avatar.h"
#include "bionics.h"
#include "calendar.h"
#include "catalua.h"
#include "catalua_hooks.h"
#include "catalua_icallback_actor.h"
#include "character.h"
#include "character_functions.h"
#include "character_martial_arts.h"
#include "character_turn.h"
#include "crafting.h"
#include "creature.h"
#include "debug.h"
#include "dialogue_win.h"
#include "effect.h"
#include "faction.h"
#include "flag.h"
#include "game.h"
#include "overmap/overmap_ui.h"
#include "character_display.h"
#include "game_inventory.h"
#include "game_constants.h"
#include "construction.h"
#include "gates.h"
#include "iexamine.h"
#include "gun_mode.h"
#include "input.h"
#include "item.h"
#include "item_handling_util.h"
#include "line.h"
#include "magic/magic.h"
#include "map/map.h"
#include "map/mapbuffer.h"
#include "map/mapdata.h"
#include "map/submap.h"
#include "monexamine.h"
#include "messages.h"
#include "mission.h"
#include "map/field_type.h"
#include "monster.h"
#include "martialarts.h"
#include "mtype.h"
#include "mutation.h"
#include "mp_common.h"
#include "mp_net.h"
#include "npc.h"
#include "npctrade.h"
#include "overmap/omdata.h"
#include "options.h"
#include "output.h"
#include "overmap/overmapbuffer.h"
#include "player_activity.h"
#include "popup.h"
#include "ranged.h"
#include "recipe.h"
#include "recipe_dictionary.h"
#include "skill.h"
#include "requirements.h"
#include "itype.h"
#include "reload/reload_selection.h"
#include "string_formatter.h"
#include "string_input_popup.h"
#include "string_utils.h"
#include "translations.h"
#include "trap.h"
#include "type_id.h"
#include "ui.h"
#include "ui_manager.h"
#include "uistate.h"
#include "vehicle/vehicle.h"
#include "weather/weather.h"
#include "vehicle/vpart_position.h"
#include "worldfactory.h"
#include "world.h"

static const bionic_id bio_cqb( "bio_cqb" );
static const efftype_id effect_npc_suspend( "npc_suspend" );
static const quality_id qual_CUT( "CUT" );
static const trait_id trait_BRAWLER( "BRAWLER" );
static const faction_id faction_your_followers( "your_followers" );

// Grants the co-op host code access to a few private parts of `game`.
struct mp_game_access {
    static void add_active_npc( game &g, const shared_ptr_fast<npc> &np ) {
        g.active_npc.push_back( np );
        np->get_mapbuffer().add_active_npc( np );
    }
    static unsigned int seed( const game &g ) {
        return g.seed;
    }
    static void set_seed( game &g, unsigned int s ) {
        g.seed = s;
    }
    static void vertical_shift( game &g, int z_before, int z_after ) {
        g.vertical_shift( z_before, z_after );
    }
};

namespace cata_mp::host
{

namespace
{

constexpr int near_scan_radius = 2;      // submaps around each player, every broadcast
constexpr int rolling_scan_budget = 48;  // extra submaps re-checked per full broadcast
constexpr int omt_sync_radius = 18;      // overmap tiles around each remote player

struct peer_t {
    int id = 0;
    std::string address;
    std::string name;
    bool probed = false;
    bool joined = false;
    character_id proxy;
    // A received, not yet executed action (raw message text).
    std::optional<std::string> action;
    bool turn_announced = false;
    int auto_wait_turns = 0;
    // The wait ends early on this: "weather" (it changes from wait_weather)
    // or "stamina" (full again).
    std::string wait_until;
    // Sound markers for the next state update.
    std::vector<tripoint_abs_ms> sound_marks;
    weather_type_id wait_weather;
    // The host chose not to wait for this player: their character idles
    // until they send an action.
    bool dont_wait = false;
    // Turns in a row the character's activity did not use any moves.
    int stalled_activity_turns = 0;
    // Hostiles already in sight when the player started waiting.
    std::set<const monster *> wait_known_hostiles;
    // Something that would interrupt the character's activity; the player is
    // asked whether to stop before the character's next turn.
    std::optional<std::pair<distraction_type, std::string>> distraction;
    // Hostiles the character saw last turn (a new one interrupts).
    int seen_hostiles = 0;
    // A hostile came into view since the player's last action: like the
    // player's safe mode, it stops a walk to a clicked spot.
    std::optional<std::string> spotted;

    // What this client already has, by content hash.
    std::map<tripoint_abs_sm, size_t> sm_sent;
    std::set<int> synced_z;
    size_t rolling_cursor = 0;
    std::map<std::string, size_t> mon_sent;
    std::map<int, size_t> npc_sent;
    size_t you_sent = 0;
    std::map<tripoint_abs_omt, std::string> omt_sent;
    size_t missions_sent = 0;
    std::optional<tripoint_abs_omt> omt_center;
    std::vector<std::pair<int, std::string>> msgs;
    // Submaps the client is known to be missing; sent with the next broadcast.
    std::set<tripoint_abs_sm> forced;
    bool full_sync = true;
};

std::map<int, peer_t> peers;

// Remote questions: while the host runs a player's action, questions go to
// that player (prompt_peer); their answers arrive by question id.
int prompt_peer = -1;
int next_question_id = 1;
// Tells the remote player's conversation windows apart.
int next_conversation_id = 0;
// The player whose turn it is (the host's name during the host's turn).
std::string acting_name;
// Raw answer messages by question id.
std::map<int, std::string> answers;

struct prompt_scope {
    int saved;
    explicit prompt_scope( int peer ) : saved( prompt_peer ) {
        prompt_peer = peer;
    }
    ~prompt_scope() {
        prompt_peer = saved;
    }
    prompt_scope( const prompt_scope & ) = delete;
    prompt_scope &operator=( const prompt_scope & ) = delete;
};

void send( int peer, const std::string &msg )
{
    net::host_send( peer, msg );
}

void send_error( int peer, const std::string &text )
{
    send( peer, "{\"t\":\"error\",\"msg\":" + json_quote( text ) + "}" );
    net::host_close_peer( peer );
}

peer_t *find_peer( int id )
{
    const auto it = peers.find( id );
    return it == peers.end() ? nullptr : &it->second;
}

peer_t *peer_of_proxy( const character_id &id )
{
    for( auto &[pid, p] : peers ) {
        if( p.joined && p.proxy == id ) {
            return &p;
        }
    }
    return nullptr;
}

npc *proxy_of( const peer_t &p )
{
    if( !p.joined ) {
        return nullptr;
    }
    return g->critter_by_id<npc>( p.proxy );
}

std::string host_name()
{
    return get_avatar().get_name();
}

bool in_safe_area( const tripoint_bub_ms &p )
{
    // Keep remote players one submap away from the edge of the reality bubble:
    // NPCs that leave it are unloaded.
    return p.x() >= SEEX && p.y() >= SEEY &&
           p.x() < SEEX * ( g_mapsize - 1 ) && p.y() < SEEY * ( g_mapsize - 1 );
}

std::optional<tripoint_bub_ms> free_spot_near( const tripoint_bub_ms &center, int radius )
{
    map &here = get_map();
    for( int r = 1; r <= radius; ++r ) {
        for( const tripoint_bub_ms &p : here.points_in_radius( center, r ) ) {
            if( rl_dist( p, center ) != r || !here.inbounds( p ) || !in_safe_area( p ) ) {
                continue;
            }
            if( here.passable( p ) && g->critter_at( p ) == nullptr && !here.has_flag( "DOOR", p ) &&
                !here.has_flag( TFLAG_SWIMMABLE, p ) && !here.has_flag( TFLAG_DEEP_WATER, p ) &&
                here.tr_at( p ).is_null() && !here.dangerous_field_at( p ) &&
                here.has_floor_or_support( p ) ) {
                return p;
            }
        }
    }
    return std::nullopt;
}

void place_npc_near_host( npc &guy )
{
    const tripoint_bub_ms center = get_avatar().bub_pos();
    const std::optional<tripoint_bub_ms> spot = free_spot_near( center, 12 );
    guy.setpos( spot ? *spot : center );
}

std::string world_name()
{
    if( world_generator && world_generator->active_world && world_generator->active_world->info ) {
        return world_generator->active_world->info->world_name;
    }
    return std::string();
}

std::vector<std::string> stored_player_names();

std::vector<std::string> proxy_names_in_world()
{
    std::vector<std::string> names;
    const auto add = [&names]( const npc & guy ) {
        const std::string n = guy.get_value( "coop_player" );
        if( !n.empty() && !guy.is_dead() &&
            std::find( names.begin(), names.end(), n ) == names.end() ) {
            names.push_back( n );
        }
    };
    for( npc &guy : g->all_npcs() ) {
        add( guy );
    }
    for( const shared_ptr_fast<npc> &guy : get_overmapbuffer( get_avatar().get_dimension() ).get_overmap_npcs() ) {
        if( guy ) {
            add( *guy );
        }
    }
    for( const std::string &name : stored_player_names() ) {
        if( std::ranges::find( names, name ) == names.end() ) {
            names.push_back( name );
        }
    }
    return names;
}

// ---- proxy setup ------------------------------------------------------------

void configure_proxy( npc &guy, const std::string &player_name )
{
    guy.set_value( "coop_player", player_name );
    guy.set_fac( faction_your_followers );
    guy.set_attitude( NPCATT_FOLLOW );
    guy.mission = NPC_MISSION_NULL;
    guy.remove_effect( effect_npc_suspend );
}

// ---- characters of players who are not playing ------------------------------
//
// Like drop-in games do, the character of a player who left is taken out of
// the world (nothing can hurt or move it) and kept here; it comes back next to
// the host when that player joins again.  The characters of players who are
// online are copied here now and then too, so a host that quits without saving
// or crashes does not cost them their progress.  Kept in the world folder;
// each entry remembers the game turn it was taken at, and the newer of an
// entry and a character found in the world wins.

constexpr const char *players_file = "coop_players.json";

struct stored_player {
    std::string json;
    int turn = 0;
};
std::map<std::string, stored_player> stored_players;
// The world the entries above belong to.
std::string stored_players_world;

void load_stored_players()
{
    if( stored_players_world == world_name() ) {
        return;
    }
    stored_players.clear();
    stored_players_world = world_name();
    world *w = g->get_active_world();
    if( w == nullptr ) {
        return;
    }
    w->read_from_file_json( players_file, []( JsonIn & jsin ) {
        JsonObject jo = jsin.get_object();
        jo.allow_omitted_members();
        for( JsonObject entry : jo.get_array( "players" ) ) {
            entry.allow_omitted_members();
            const std::string name = entry.get_string( "name", "" );
            if( name.empty() || !entry.has_object( "char" ) ) {
                continue;
            }
            stored_players[name] = stored_player{
                .json = entry.get_object( "char" ).str(),
                .turn = entry.get_int( "turn", 0 ),
            };
        }
    }, true );
}

void save_stored_players()
{
    world *w = g->get_active_world();
    if( w == nullptr ) {
        return;
    }
    w->write_to_file( players_file, []( std::ostream & fout ) {
        JsonOut jsout( fout );
        jsout.start_object();
        jsout.member( "players" );
        jsout.start_array();
        for( const auto &[name, entry] : stored_players ) {
            jsout.start_object();
            jsout.member( "name", name );
            jsout.member( "turn", entry.turn );
            jsout.member( "char" );
            jsout.write_separator();
            fout << entry.json;
            jsout.set_need_separator();
            jsout.end_object();
        }
        jsout.end_array();
        jsout.end_object();
    }, _( "co-op players" ) );
}

// Copies a player's character into the store (unless the store has a newer
// copy, taken after the world was last saved).
void store_player( const npc &guy )
{
    const std::string name = guy.get_value( "coop_player" );
    if( name.empty() || guy.is_dead() ) {
        return;
    }
    load_stored_players();
    stored_player &slot = stored_players[name];
    const int now = to_turn<int>( calendar::turn );
    if( slot.json.empty() || slot.turn <= now ) {
        slot.json = write_json( [&guy]( JsonOut & jsout ) {
            guy.serialize( jsout );
        } );
        slot.turn = now;
    }
}

// Takes the character of a player who is not playing out of the world.
void stash_proxy( const character_id &id )
{
    overmapbuffer &omb = get_overmapbuffer( get_avatar().get_dimension() );
    // Keeps the character alive while it is taken out.
    const shared_ptr_fast<npc> guy = omb.find_npc( id );
    if( !guy ) {
        return;
    }
    store_player( *guy );
    const bool active = std::ranges::any_of( g->all_npcs(), [&id]( const npc & n ) {
        return n.getID() == id;
    } );
    if( active ) {
        g->erase_npc( id );
    }
    omb.remove_npc( id );
    save_stored_players();
    log( string_format( "stored the character of %s", guy->get_value( "coop_player" ) ) );
}

// Every character of a player who is not playing goes into the store.
void stash_offline_proxies()
{
    std::vector<character_id> offline;
    for( npc &guy : g->all_npcs() ) {
        if( is_proxy( guy ) && !guy.is_dead() && peer_of_proxy( guy.getID() ) == nullptr ) {
            offline.push_back( guy.getID() );
        }
    }
    for( const character_id &id : offline ) {
        stash_proxy( id );
    }
}

// A copy of everybody who is playing, in case the host never saves.
void snapshot_online_players()
{
    for( const auto &[id, p] : peers ) {
        if( const npc *guy = p.joined ? proxy_of( p ) : nullptr ) {
            store_player( *guy );
        }
    }
    save_stored_players();
}

std::vector<std::string> stored_player_names()
{
    load_stored_players();
    std::vector<std::string> names;
    for( const auto &[name, entry] : stored_players ) {
        names.push_back( name );
    }
    return names;
}

npc *create_proxy( JsonIn &char_json, const std::string &player_name );

// Brings a stored character back, next to the host.
npc *restore_stored_proxy( const std::string &player_name )
{
    load_stored_players();
    const auto it = stored_players.find( player_name );
    if( it == stored_players.end() ) {
        return nullptr;
    }
    npc *guy = nullptr;
    try {
        std::istringstream is( it->second.json );
        JsonIn jsin( is );
        guy = create_proxy( jsin, player_name );
    } catch( const std::exception &e ) {
        log( std::string( "restoring a stored character failed: " ) + e.what() );
        return nullptr;
    }
    if( guy != nullptr ) {
        stored_players.erase( it );
        save_stored_players();
    }
    return guy;
}

// Finds the proxy of `player_name`, bringing it into the reality bubble next to
// the host if it was stored on the overmap.  Returns nullptr if unknown.
npc *activate_existing_proxy( const std::string &player_name )
{
    for( npc &guy : g->all_npcs() ) {
        if( guy.get_value( "coop_player" ) == player_name && !guy.is_dead() ) {
            if( !in_safe_area( guy.bub_pos() ) || rl_dist( guy.bub_pos(), get_avatar().bub_pos() ) > 60 ) {
                place_npc_near_host( guy );
            }
            return &guy;
        }
    }
    if( npc *restored = restore_stored_proxy( player_name ) ) {
        return restored;
    }
    overmapbuffer &omb = get_overmapbuffer( get_avatar().get_dimension() );
    for( const shared_ptr_fast<npc> &stored : omb.get_overmap_npcs() ) {
        if( !stored || stored->is_dead() || stored->get_value( "coop_player" ) != player_name ) {
            continue;
        }
        const character_id id = stored->getID();
        shared_ptr_fast<npc> guy = omb.remove_npc( id );
        if( !guy ) {
            return nullptr;
        }
        const tripoint_abs_ms target = get_avatar().abs_pos();
        const auto proj = project_remain<coords::sm>( target );
        guy->spawn_at_precise( proj.quotient, proj.remainder_tripoint );
        omb.insert_npc( guy );
        g->load_npcs();
        npc *active = g->critter_by_id<npc>( id );
        if( active != nullptr ) {
            place_npc_near_host( *active );
        }
        return active;
    }
    return nullptr;
}

npc *create_proxy( JsonIn &char_json, const std::string &player_name )
{
    shared_ptr_fast<npc> guy = make_shared_fast<npc>();
    guy->deserialize( char_json );
    guy->setID( g->assign_npc_id(), true );
    guy->set_dimension( get_avatar().get_dimension() );
    const tripoint_abs_ms target = get_avatar().abs_pos();
    const auto proj = project_remain<coords::sm>( target );
    guy->spawn_at_precise( proj.quotient, proj.remainder_tripoint );
    configure_proxy( *guy, player_name );
    for( item *it : guy->inv_dump() ) {
        it->set_owner( *guy );
    }
    get_overmapbuffer( get_avatar().get_dimension() ).insert_npc( guy );
    g->load_npcs();
    npc *active = g->critter_by_id<npc>( guy->getID() );
    if( active != nullptr ) {
        place_npc_near_host( *active );
    }
    return active;
}

// ---- state serialization ------------------------------------------------------

std::string serialize_submap( const tripoint_abs_sm &p, const submap &sm )
{
    return write_json( [&]( JsonOut & jsout ) {
        jsout.start_object();
        jsout.member( "version", savegame_version );
        jsout.member( "coordinates" );
        jsout.start_array();
        jsout.write( p.x() );
        jsout.write( p.y() );
        jsout.write( p.z() );
        jsout.end_array();
        sm.store( jsout );
        jsout.end_object();
    } );
}

// Hash of a serialized submap that ignores the bookkeeping timestamps and the
// ambient temperature written before the terrain.
size_t submap_content_hash( const std::string &json )
{
    const size_t pos = json.find( "\"terrain\"" );
    return std::hash<std::string_view>()( pos == std::string::npos ? std::string_view( json ) :
                                          std::string_view( json ).substr( pos ) );
}

// Shared by the states built in one broadcast: with several players each
// creature is serialized once, not once per player.
struct broadcast_cache {
    std::map<tripoint_abs_sm, std::pair<size_t, std::string>> submaps;
    std::map<const monster *, std::pair<size_t, std::string>> monsters;
    std::map<int, std::pair<size_t, std::string>> characters;
};

const std::pair<size_t, std::string> *cached_submap( broadcast_cache &cache,
        const tripoint_abs_sm &p )
{
    const auto it = cache.submaps.find( p );
    if( it != cache.submaps.end() ) {
        return it->second.second.empty() ? nullptr : &it->second;
    }
    submap *sm = get_map().get_mapbuffer().lookup_submap_in_memory( p );
    if( sm == nullptr ) {
        cache.submaps.emplace( p, std::make_pair( 0, std::string() ) );
        return nullptr;
    }
    std::string json = serialize_submap( p, *sm );
    const size_t h = submap_content_hash( json );
    auto &slot = cache.submaps[p];
    slot = std::make_pair( h, std::move( json ) );
    return &slot;
}

enum class sync_level : int {
    light,   // creatures, own character, messages, submaps right around players
    normal,  // + submaps within near_scan_radius of every player
    full,    // + a slice of the whole bubble
};

void add_submaps_around( std::set<tripoint_abs_sm> &out, const tripoint_abs_ms &pos, int radius,
                         const std::set<int> &zs )
{
    const tripoint_abs_sm center = project_to<coords::sm>( pos );
    for( int z : zs ) {
        for( int dx = -radius; dx <= radius; ++dx ) {
            for( int dy = -radius; dy <= radius; ++dy ) {
                out.insert( tripoint_abs_sm( center.x() + dx, center.y() + dy, z ) );
            }
        }
    }
}

std::string build_state( peer_t &p, sync_level level, broadcast_cache &cache )
{
    npc *proxy = proxy_of( p );
    if( proxy == nullptr ) {
        return std::string();
    }
    avatar &u = get_avatar();
    map &here = get_map();
    std::string out = "{\"t\":\"state\",\"turn\":" + std::to_string( to_turn<int>( calendar::turn ) );
    {
        const weather_manager &w = get_weather();
        out += string_format( ",\"weather\":[%s,%d,%d,%d]", json_quote( w.weather_id.str() ),
                              static_cast<int>( units::to_millidegree_celsius( w.temperature ) ),
                              w.windspeed, w.winddirection );
    }
    const tripoint_abs_ms center = u.abs_pos();
    out += string_format( ",\"center\":[%d,%d,%d]", center.x(), center.y(), center.z() );
    // Whose turn it is, and who plays (the host first).
    out += ",\"acting\":" + json_quote( acting_name.empty() ? host_name() : acting_name );
    out += ",\"players\":" + write_json( []( JsonOut & jsout ) {
        jsout.start_array();
        jsout.write( host_name() );
        for( const std::string &name : joined_names() ) {
            jsout.write( name );
        }
        jsout.end_array();
    } );

    // ---- own character
    {
        const std::string you = write_json( [proxy]( JsonOut & jsout ) {
            proxy->serialize( jsout );
        } );
        // Whether the character drives is saved for the avatar only.
        const std::string driving = proxy->controlling_vehicle ? "1" : "0";
        const size_t h = std::hash<std::string>()( you + driving );
        if( h != p.you_sent || p.full_sync ) {
            p.you_sent = h;
            out += ",\"you\":" + you + ",\"driving\":" + ( proxy->controlling_vehicle ? "true" : "false" );
        }
        // Whether the character is busy with something (sent every time:
        // the client relies on it rather than on its own copy).
        out += string_format( ",\"busy\":%s", proxy->activity && *proxy->activity ? "true" : "false" );
    }

    // ---- vehicles' fuel use (the client does not run vehicles, but shows
    // how long the fuel lasts from this)
    {
        std::string fuel;
        for( const wrapped_vehicle &wv : get_map().get_vehicles() ) {
            const vehicle &veh = *wv.v;
            const tripoint_abs_ms at = veh.abs_ms_location();
            for( const auto &[id, used] : veh.fuel_used_last_turn ) {
                fuel += fuel.empty() ? "" : ",";
                fuel += string_format( "[%d,%d,%d,%s,%.4f]", at.x(), at.y(), at.z(),
                                       json_quote( id.str() ), used );
            }
        }
        if( !fuel.empty() ) {
            out += ",\"fuel\":[" + fuel + "]";
        }
    }

    // ---- monsters
    {
        std::map<std::string, size_t> now;
        std::string upd;
        std::string keys;
        for( monster &critter : g->all_monsters() ) {
            if( critter.is_dead() ) {
                continue;
            }
            const std::string key = std::to_string( reinterpret_cast<uintptr_t>( &critter ) );
            auto [cached, fresh] = cache.monsters.try_emplace( &critter );
            if( fresh ) {
                cached->second.second = write_json( [&critter]( JsonOut & jsout ) {
                    critter.serialize( jsout );
                } );
                cached->second.first = std::hash<std::string>()( cached->second.second );
            }
            const std::string &json = cached->second.second;
            const size_t h = cached->second.first;
            now[key] = h;
            const auto old = p.mon_sent.find( key );
            if( p.full_sync || old == p.mon_sent.end() || old->second != h ) {
                upd += upd.empty() ? "" : ",";
                upd += json;
                keys += keys.empty() ? "" : ",";
                keys += json_quote( key );
            }
        }
        std::string del;
        for( const auto &[key, h] : p.mon_sent ) {
            if( !now.contains( key ) ) {
                del += del.empty() ? "" : ",";
                del += json_quote( key );
            }
        }
        if( p.full_sync ) {
            out += ",\"mon_reset\":true";
        }
        if( !upd.empty() ) {
            out += ",\"mon_keys\":[" + keys + "],\"mon\":[" + upd + "]";
        }
        if( !del.empty() ) {
            out += ",\"mon_del\":[" + del + "]";
        }
        p.mon_sent = std::move( now );
    }

    // ---- other characters: the host avatar, other remote players, NPCs
    {
        std::map<int, size_t> now;
        std::string upd;
        std::string keys;
        const auto consider = [&]( Character & who, const std::function<void( JsonOut & )> &writer ) {
            const int key = who.getID().get_value();
            auto [cached, fresh] = cache.characters.try_emplace( key );
            if( fresh ) {
                cached->second.second = write_json( writer );
                cached->second.first = std::hash<std::string>()( cached->second.second );
            }
            const std::string &json = cached->second.second;
            const size_t h = cached->second.first;
            now[key] = h;
            const auto old = p.npc_sent.find( key );
            if( p.full_sync || old == p.npc_sent.end() || old->second != h ) {
                upd += upd.empty() ? "" : ",";
                upd += json;
                keys += keys.empty() ? "" : ",";
                keys += std::to_string( key );
            }
        };
        consider( u, [&u]( JsonOut & jsout ) {
            u.serialize( jsout );
        } );
        for( npc &guy : g->all_npcs() ) {
            if( guy.getID() == p.proxy || guy.is_dead() ) {
                continue;
            }
            consider( guy, [&guy]( JsonOut & jsout ) {
                guy.serialize( jsout );
            } );
        }
        std::string del;
        for( const auto &[key, h] : p.npc_sent ) {
            if( !now.contains( key ) ) {
                del += del.empty() ? "" : ",";
                del += std::to_string( key );
            }
        }
        if( p.full_sync ) {
            out += ",\"npc_reset\":true";
        }
        out += string_format( ",\"host_id\":%d", u.getID().get_value() );
        if( !upd.empty() ) {
            out += ",\"npc_keys\":[" + keys + "],\"npc\":[" + upd + "]";
        }
        if( !del.empty() ) {
            out += ",\"npc_del\":[" + del + "]";
        }
        p.npc_sent = std::move( now );
    }

    // ---- submaps
    {
        const int pz = proxy->abs_pos().z();
        std::set<int> zs;
        for( int z = std::max( -OVERMAP_DEPTH, pz - 1 ); z <= std::min( OVERMAP_HEIGHT, pz + 1 ); ++z ) {
            zs.insert( z );
        }
        zs.insert( u.abs_pos().z() );

        // Everything above the player matters for sunlight and for what can
        // be seen from rooftops; those levels are sent whole once and then
        // only rechecked by the rolling scan.
        std::set<int> whole_levels = zs;
        for( int z = std::max( -OVERMAP_DEPTH, pz - 1 ); z <= OVERMAP_HEIGHT; ++z ) {
            whole_levels.insert( z );
        }
        std::set<tripoint_abs_sm> candidates;
        const point_abs_sm origin = here.get_abs_sub();
        const auto add_level = [&]( int z ) {
            for( int x = 0; x < g_mapsize; ++x ) {
                for( int y = 0; y < g_mapsize; ++y ) {
                    candidates.insert( tripoint_abs_sm( origin.x() + x, origin.y() + y, z ) );
                }
            }
        };
        // Newly visited z-levels (and everything on a full resync) go out whole.
        for( int z : whole_levels ) {
            if( p.full_sync || !p.synced_z.contains( z ) ) {
                add_level( z );
                p.synced_z.insert( z );
            }
        }
        for( const tripoint_abs_sm &f : p.forced ) {
            candidates.insert( f );
        }
        p.forced.clear();
        const int radius = level == sync_level::light ? 1 : near_scan_radius;
        add_submaps_around( candidates, proxy->abs_pos(), radius, zs );
        add_submaps_around( candidates, u.abs_pos(), radius, { u.abs_pos().z() } );
        if( level == sync_level::full ) {
            std::vector<tripoint_abs_sm> all;
            for( int z : p.synced_z ) {
                for( int x = 0; x < g_mapsize; ++x ) {
                    for( int y = 0; y < g_mapsize; ++y ) {
                        all.emplace_back( origin.x() + x, origin.y() + y, z );
                    }
                }
            }
            if( !all.empty() ) {
                for( int i = 0; i < rolling_scan_budget; ++i ) {
                    candidates.insert( all[( p.rolling_cursor + i ) % all.size()] );
                }
                p.rolling_cursor = ( p.rolling_cursor + rolling_scan_budget ) % all.size();
            }
        }
        std::string sms;
        int count = 0;
        for( const tripoint_abs_sm &sp : candidates ) {
            if( !here.inbounds( sp ) ) {
                continue;
            }
            const auto *entry = cached_submap( cache, sp );
            if( entry == nullptr ) {
                continue;
            }
            const auto old = p.sm_sent.find( sp );
            if( old != p.sm_sent.end() && old->second == entry->first ) {
                continue;
            }
            p.sm_sent[sp] = entry->first;
            sms += sms.empty() ? "" : ",";
            sms += entry->second;
            ++count;
        }
        if( !sms.empty() ) {
            out += ",\"sm\":[" + sms + "]";
        }
    }

    // ---- sounds heard where the source is out of sight
    if( !p.sound_marks.empty() ) {
        std::string marks;
        for( const tripoint_abs_ms &m : p.sound_marks ) {
            marks += marks.empty() ? "" : ",";
            marks += string_format( "[%d,%d,%d]", m.x(), m.y(), m.z() );
        }
        out += ",\"smark\":[" + marks + "]";
        p.sound_marks.clear();
    }

    // ---- overmap terrain around the remote player
    {
        const tripoint_abs_omt omt = proxy->abs_omt_pos();
        if( p.full_sync || !p.omt_center || *p.omt_center != omt ) {
            p.omt_center = omt;
            overmapbuffer &omb = get_overmapbuffer( proxy->get_dimension() );
            std::string omts;
            for( int dx = -omt_sync_radius; dx <= omt_sync_radius; ++dx ) {
                for( int dy = -omt_sync_radius; dy <= omt_sync_radius; ++dy ) {
                    const tripoint_abs_omt op( omt.x() + dx, omt.y() + dy, omt.z() );
                    const std::string id = omb.ter( op ).id().str();
                    auto &slot = p.omt_sent[op];
                    if( slot == id ) {
                        continue;
                    }
                    slot = id;
                    omts += omts.empty() ? "" : ",";
                    omts += string_format( "[%d,%d,%d,%s]", op.x(), op.y(), op.z(), json_quote( id ) );
                }
            }
            if( !omts.empty() ) {
                out += ",\"omt\":[" + omts + "]";
            }
        }
    }

    // ---- missions: shared by all players, kept in the avatar's journal
    {
        const std::string missions = write_json( [&u]( JsonOut & jsout ) {
            jsout.start_object();
            jsout.member( "world" );
            mission::serialize_all( jsout );
            jsout.member( "active", mission::to_uid_vector( u.get_active_missions() ) );
            jsout.member( "completed", mission::to_uid_vector( u.get_completed_missions() ) );
            jsout.member( "failed", mission::to_uid_vector( u.get_failed_missions() ) );
            const mission *current = u.get_active_mission();
            jsout.member( "current", current != nullptr ? current->get_id() : -1 );
            jsout.end_object();
        } );
        const size_t h = std::hash<std::string>()( missions );
        if( h != p.missions_sent || p.full_sync ) {
            p.missions_sent = h;
            out += ",\"missions\":" + missions;
        }
    }

    // ---- messages
    if( !p.msgs.empty() ) {
        std::string msgs;
        for( const auto &[type, text] : p.msgs ) {
            msgs += msgs.empty() ? "" : ",";
            msgs += string_format( "[%d,%s]", type, json_quote( text ) );
        }
        out += ",\"msg\":[" + msgs + "]";
        p.msgs.clear();
    }

    p.full_sync = false;
    out += "}";
    return out;
}

void broadcast( sync_level level )
{
    if( peers.empty() ) {
        return;
    }
    broadcast_cache cache;
    for( auto &[id, p] : peers ) {
        if( !p.joined ) {
            continue;
        }
        const std::string state = build_state( p, level, cache );
        if( !state.empty() ) {
            send( id, state );
        }
    }
}

void send_to_player( peer_t &p, game_message_type type, const std::string &text )
{
    p.msgs.emplace_back( static_cast<int>( type ), text );
}

// ---- handshake ----------------------------------------------------------------

void handle_probe( peer_t &p, message &m )
{
    JsonObject jo = m.object();
    const std::string ver = jo.get_string( "ver", "" );
    const std::string pw = jo.get_string( "pw", "" );
    if( ver != protocol_version_string() ) {
        log( string_format( "peer %d (%s): version mismatch, theirs '%s' ours '%s'", p.id, p.address,
                            ver, protocol_version_string() ) );
        send_error( p.id, string_format(
                        _( "Version mismatch.\nHost: %s\nYou: %s\nBoth players must run the same build." ),
                        protocol_version_string(), ver ) );
        return;
    }
    if( !settings().password.empty() && pw != settings().password ) {
        log( string_format( "peer %d (%s): wrong password", p.id, p.address ) );
        send_error( p.id, _( "Wrong password." ) );
        return;
    }
    p.probed = true;
    const WORLDINFO *info = world_generator->active_world->info;
    std::string out = "{\"t\":\"welcome\"";
    out += ",\"world\":" + json_quote( world_name() );
    out += ",\"host\":" + json_quote( host_name() );
    out += ",\"seed\":" + json_quote( std::to_string( mp_game_access::seed( *g ) ) );
    out += ",\"mods\":" + write_json( [info]( JsonOut & jsout ) {
        jsout.start_array();
        for( const mod_id &m : info->active_mod_order ) {
            jsout.write( m.str() );
        }
        jsout.end_array();
    } );
    out += ",\"options\":" + write_json( [info]( JsonOut & jsout ) {
        jsout.start_object();
        for( const auto &[name, opt] : info->WORLD_OPTIONS ) {
            jsout.member( name, opt.getValue( true ) );
        }
        jsout.end_object();
    } );
    out += ",\"players\":" + write_json( []( JsonOut & jsout ) {
        jsout.write( proxy_names_in_world() );
    } );
    out += "}";
    send( p.id, out );
    log( string_format( "peer %d (%s): probe accepted", p.id, p.address ) );
}

void handle_join( peer_t &p, message &m )
{
    JsonObject jo = m.object();
    if( !p.probed ) {
        send_error( p.id, _( "Handshake error." ) );
        return;
    }
    std::string name = trim( jo.get_string( "name", "" ) );
    if( name.empty() ) {
        send_error( p.id, _( "No character name given." ) );
        return;
    }
    for( const auto &[id, other] : peers ) {
        if( other.joined && other.name == name ) {
            send_error( p.id, string_format( _( "%s is already playing in this session." ), name ) );
            return;
        }
    }
    if( name == host_name() ) {
        send_error( p.id, _( "That is the host's character name.  Choose another name." ) );
        return;
    }

    npc *proxy = activate_existing_proxy( name );
    if( proxy == nullptr ) {
        if( !jo.has_member( "char" ) || jo.has_null( "char" ) ) {
            send_error( p.id, string_format(
                            _( "There is no character called %s near the host.  Create a new character instead." ),
                            name ) );
            return;
        }
        try {
            JsonIn *ci = jo.get_raw( "char" );
            proxy = create_proxy( *ci, name );
        } catch( const std::exception &e ) {
            log( std::string( "create_proxy failed: " ) + e.what() );
            proxy = nullptr;
        }
        if( proxy == nullptr ) {
            send_error( p.id, _( "The host could not create your character." ) );
            return;
        }
        add_msg( m_good, _( "Co-op: %s arrives." ), name );
    } else {
        add_msg( m_good, _( "Co-op: %s is back." ), name );
    }
    configure_proxy( *proxy, name );
    proxy->name = name;

    p.name = name;
    p.joined = true;
    p.proxy = proxy->getID();
    p.full_sync = true;
    p.turn_announced = false;
    p.action.reset();
    p.auto_wait_turns = 0;
    p.sm_sent.clear();
    p.synced_z.clear();
    p.omt_sent.clear();
    p.omt_center.reset();

    send( p.id, "{\"t\":\"joined\",\"name\":" + json_quote( name ) + ",\"host\":" +
          json_quote( host_name() ) + "}" );
    log( string_format( "peer %d (%s): joined as '%s'", p.id, p.address, name ) );
    for( auto &[id, other] : peers ) {
        if( other.joined && id != p.id ) {
            send_to_player( other, m_good, string_format( _( "%s joined the game." ), name ) );
        }
    }
    broadcast_cache cache;
    send( p.id, build_state( p, sync_level::full, cache ) );
}

void park_proxy( const peer_t &p )
{
    if( npc *proxy = proxy_of( p ) ) {
        // Until the end of the turn, when it is taken out of the world, an
        // offline player's character stays where it is, out of time.
        proxy->add_effect( effect_npc_suspend, time_duration::from_turns( calendar::INDEFINITELY_LONG ) );
        proxy->cancel_activity();
    }
}

// Kicks a player.  The goodbye tells the client this was deliberate, so it
// does not reconnect on its own as it does after a dropped connection.
void disconnect_peer( peer_t &p, const std::string &reason )
{
    send( p.id, "{\"t\":\"bye\",\"msg\":" + json_quote( reason ) + "}" );
    if( p.joined ) {
        park_proxy( p );
        add_msg( m_info, _( "Co-op: %s was disconnected." ), p.name );
        p.joined = false;
    }
    net::host_close_peer( p.id );
}

// ---- actions --------------------------------------------------------------

struct action_result {
    bool ok = true;
    std::string msg;
};

action_result fail( const std::string &msg )
{
    return action_result{ false, msg };
}

tripoint_bub_ms read_bub( const JsonObject &jo )
{
    const tripoint_abs_ms abs( jo.get_int( "x" ), jo.get_int( "y" ), jo.get_int( "z" ) );
    return abs_to_bub( abs );
}

// Grabbing furniture or a vehicle to drag it (G), as game.cpp's grab().
action_result do_grab( npc &guy, const JsonObject &jo )
{
    map &here = get_map();
    if( guy.get_grab_type() != OBJECT_NONE || jo.get_bool( "release", false ) ) {
        if( const auto target = vehicle_grab_target_at( here, guy.bub_pos() + guy.grab_point ) ) {
            guy.add_msg_if_player( _( "You release the %s." ), target->vp.vehicle().name );
        } else if( here.has_furn( guy.bub_pos() + guy.grab_point ) ) {
            guy.add_msg_if_player( _( "You release the %s." ),
                                   here.furnname( guy.bub_pos() + guy.grab_point ) );
        }
        guy.grab( OBJECT_NONE );
        return action_result{};
    }
    if( guy.is_mounted() ) {
        return fail( _( "You can't grab things while you're riding." ) );
    }
    const tripoint_bub_ms grabp = read_bub( jo );
    if( !here.inbounds( grabp ) || rl_dist( guy.bub_pos(), grabp ) != 1 ) {
        return fail( _( "There's nothing to grab there!" ) );
    }
    if( const auto target = vehicle_grab_target_at( here, grabp ) ) {
        guy.grab( OBJECT_VEHICLE, target->pos - guy.bub_pos() );
        guy.add_msg_if_player( _( "You grab the %s." ), target->vp.vehicle().name );
    } else if( here.has_furn( grabp ) ) {
        if( !here.furn( grabp ).obj().is_movable() ) {
            return fail( string_format( _( "You can not grab the %s" ), here.furnname( grabp ) ) );
        }
        guy.grab( OBJECT_FURNITURE, grabp - guy.bub_pos() );
        if( !here.can_move_furniture( grabp, &guy ) ) {
            guy.add_msg_if_player( _( "You grab the %s. It feels really heavy." ), here.furnname( grabp ) );
        } else {
            guy.add_msg_if_player( _( "You grab the %s." ), here.furnname( grabp ) );
        }
    } else {
        return fail( _( "There's nothing to grab there!" ) );
    }
    return action_result{};
}

// Hauling items along while walking (\), as game.cpp's haul().
action_result do_haul( npc &guy )
{
    map &here = get_map();
    if( guy.is_hauling() ) {
        guy.stop_hauling();
        return action_result{};
    }
    if( here.veh_at( guy.bub_pos() ) ) {
        return fail( _( "You cannot haul inside vehicles." ) );
    }
    if( here.has_flag( TFLAG_DEEP_WATER, guy.bub_pos() ) ) {
        return fail( _( "You cannot haul while in deep water." ) );
    }
    if( !here.can_put_items( guy.bub_pos() ) ) {
        return fail( _( "You cannot haul items here." ) );
    }
    const map_stack items = here.i_at( guy.bub_pos() );
    if( std::none_of( items.begin(), items.end(), []( const item * it ) {
    return !it->made_of( LIQUID );
    } ) ) {
        return fail( _( "There are no items to haul here." ) );
    }
    guy.start_hauling();
    return action_result{};
}

// After a step while hauling: the items left behind come along, as
// game::start_hauling does for the player.
void continue_hauling( npc &guy, const tripoint_bub_ms &from )
{
    map &here = get_map();
    if( !here.can_put_items( guy.bub_pos() ) || here.veh_at( guy.bub_pos() ) ||
        here.has_flag( TFLAG_DEEP_WATER, guy.bub_pos() ) ) {
        guy.stop_hauling();
        return;
    }
    std::vector<item *> targets;
    std::vector<int> quantities;
    for( item *const &it : here.i_at( from ) ) {
        if( it->made_of( LIQUID ) ) {
            continue;
        }
        targets.push_back( it );
        quantities.push_back( 0 );
    }
    if( targets.empty() ) {
        guy.stop_hauling();
        return;
    }
    guy.assign_activity( std::make_unique<player_activity>( std::make_unique<move_items_activity_actor>(
                             targets, quantities, false, tripoint_rel_ms() ) ) );
}

action_result do_move( npc &guy, int dx, int dy )
{
    // The sprite looks the way the player moves, as the avatar's does.
    if( dx > 0 ) {
        guy.facing = FD_RIGHT;
    } else if( dx < 0 ) {
        guy.facing = FD_LEFT;
    }
    map &here = get_map();
    // Dragging what we hold: it moves first (or blocks the step), as in
    // game::walk_move.
    if( guy.get_grab_type() != OBJECT_NONE &&
        g->grabbed_move( tripoint_rel_ms( dx, dy, 0 ), false, &guy ) ) {
        return action_result{};
    }
    const tripoint_bub_ms dest = guy.bub_pos() + tripoint_rel_ms( dx, dy, 0 );
    if( !here.inbounds( dest ) || !in_safe_area( dest ) ) {
        return fail( string_format( _( "You can't go any further away from %s." ), host_name() ) );
    }
    if( Creature *critter = g->critter_at( dest ) ) {
        if( critter == &guy ) {
            return fail( std::string() );
        }
        const bool hostile = critter->is_monster() ?
                             critter->as_monster()->attitude_to( guy ) == Attitude::A_HOSTILE ||
                             !critter->as_monster()->is_pet() :
                             guy.attitude_to( *critter ) == Attitude::A_HOSTILE;
        if( hostile ) {
            guy.melee_attack( *critter, true );
            if( npc *other = critter->as_npc() ) {
                // Like the player's attack: it angers them even on a miss.
                other->on_attacked( guy );
            }
            return action_result{};
        }
        // Friends and pets step aside, like followers do for the player.
        const bool can_swap = critter->is_monster() ? critter->as_monster()->is_pet() :
                              ( !critter->as_character()->in_sleep_state() || is_proxy( *critter->as_character() ) );
        if( can_swap && here.passable( guy.bub_pos() ) ) {
            // disp_name() of the host's avatar is "you".
            const std::string other = critter->is_avatar() ? critter->as_avatar()->get_name() :
                                      critter->disp_name( false, true );
            if( g->swap_critters( guy, *critter ) ) {
                guy.mod_moves( -100 );
                guy.add_msg_if_player( _( "You swap places with %s." ), other );
                if( critter->is_avatar() ) {
                    add_msg( _( "%s swaps places with you." ), guy.get_name() );
                } else {
                    critter->add_msg_if_player( _( "%s swaps places with you." ), guy.get_name() );
                }
                return action_result{};
            }
        }
        return fail( string_format( _( "%s is in the way." ), critter->disp_name( false, true ) ) );
    }
    const bool inside = !here.is_outside( guy.bub_pos() );
    if( !here.passable( dest ) && !here.can_open_door( static_cast<const Character *>( &guy ), dest, inside ) ) {
        if( const optional_vpart_position vp = here.veh_at( dest ) ) {
            const int openable = vp->vehicle().next_part_to_open( vp->part_index() );
            if( openable >= 0 && here.open_door_veh( static_cast<Character *>( &guy ), vp, dest, inside ) ) {
                guy.mod_moves( -100 );
                return action_result{};
            }
        }
        if( here.has_flag( "LOCKED", dest ) ) {
            return fail( _( "The door is locked!" ) );
        }
        return fail( string_format( _( "There is a %s in the way." ), here.obstacle_name( dest ) ) );
    }
    const int moves_before = guy.get_moves();
    const tripoint_bub_ms from = guy.bub_pos();
    guy.move_to( dest, true );
    if( guy.get_moves() == moves_before && guy.bub_pos() != dest ) {
        return fail( _( "You can't move there." ) );
    }
    // Walking and running cost stamina, as they do for the player.
    if( guy.bub_pos() == dest ) {
        guy.burn_move_stamina( moves_before - guy.get_moves() );
        // As game::walk_move tells the player.
        if( here.has_flag_ter_or_furn( TFLAG_HIDE_PLACE, dest ) &&
            guy.get_size() <= creature_size::medium ) {
            guy.add_msg_if_player( m_good, _( "You are hiding in the %s." ), here.name( dest ) );
        }
        if( guy.is_hauling() ) {
            continue_hauling( guy, from );
        }
    }
    return action_result{};
}

// A step of a walk to a clicked spot stops where the player's would: at a
// creature in the way (instead of attacking or pushing past it) and when a
// hostile comes into view.
bool auto_move_stopped( npc &guy, const peer_t &p, const tripoint_bub_ms &dest )
{
    if( p.spotted ) {
        guy.add_msg_if_player( m_warning, *p.spotted );
        guy.add_msg_if_player( m_warning, _( "Auto-move canceled." ) );
        return true;
    }
    Creature *critter = g->critter_at( dest );
    if( critter == nullptr || critter == &guy ) {
        return false;
    }
    if( const monster *mon = critter->as_monster() ) {
        if( mon->friendly != 0 || mon->is_pet() || mon->attitude( &guy ) == MATT_FRIEND ) {
            return false;
        }
        guy.add_msg_if_player( m_warning, _( "Monster in the way.  Auto-move canceled." ) );
        guy.add_msg_if_player( m_info, _( "Move into the monster to attack." ) );
        return true;
    }
    guy.add_msg_if_player( _( "NPC in the way, Auto-move canceled." ) );
    guy.add_msg_if_player( m_info, _( "Move into the NPC to interact or attack." ) );
    return true;
}

action_result do_vertical_move( npc &guy, int dz )
{
    map &here = get_map();
    const tripoint_bub_ms pos = guy.bub_pos();
    const bool up = dz > 0;
    if( up && !here.has_flag( "GOES_UP", pos ) ) {
        return fail( _( "You can't go up here!" ) );
    }
    if( !up && !here.has_flag( "GOES_DOWN", pos ) ) {
        return fail( _( "You can't go down here!" ) );
    }
    const int new_z = pos.z() + ( up ? 1 : -1 );
    if( new_z < -OVERMAP_DEPTH || new_z > OVERMAP_HEIGHT ) {
        return fail( _( "You can't go there." ) );
    }
    const tripoint_bub_ms straight( pos.x(), pos.y(), new_z );
    std::optional<tripoint_bub_ms> best;
    int best_dist = INT_MAX;
    const std::string counterpart = up ? "GOES_DOWN" : "GOES_UP";
    for( const tripoint_bub_ms &p : here.points_in_radius( straight, 4 ) ) {
        if( p.z() != new_z || !here.inbounds( p ) || !here.has_flag( counterpart, p ) ||
            !here.passable( p ) || g->critter_at( p ) != nullptr ) {
            continue;
        }
        const int d = rl_dist( p, straight );
        if( d < best_dist ) {
            best_dist = d;
            best = p;
        }
    }
    if( !best && here.passable( straight ) && g->critter_at( straight ) == nullptr ) {
        best = straight;
    }
    if( !best || !in_safe_area( *best ) ) {
        return fail( _( "The way is blocked." ) );
    }
    guy.setpos( *best );
    guy.mod_moves( -100 );
    guy.add_msg_if_player( up ? _( "You climb up." ) : _( "You climb down." ) );
    return action_result{};
}

action_result do_open( npc &guy, const tripoint_bub_ms &pos )
{
    map &here = get_map();
    if( rl_dist( guy.bub_pos(), pos ) > 1 || !here.inbounds( pos ) ) {
        return fail( _( "That is too far away." ) );
    }
    const bool inside = !here.is_outside( guy.bub_pos() );
    if( const optional_vpart_position vp = here.veh_at( pos ) ) {
        const int openable = vp->vehicle().next_part_to_open( vp->part_index() );
        if( openable >= 0 ) {
            if( here.open_door_veh( static_cast<Character *>( &guy ), vp, pos, inside ) ) {
                guy.mod_moves( -100 );
                return action_result{};
            }
            return fail( _( "You can't open that." ) );
        }
    }
    if( here.open_door( static_cast<Character *>( &guy ), pos, inside ) ) {
        guy.mod_moves( -100 );
        return action_result{};
    }
    if( here.has_flag( "LOCKED", pos ) ) {
        return fail( _( "The door is locked!" ) );
    }
    return fail( _( "No door there." ) );
}

action_result do_close( npc &guy, const tripoint_bub_ms &pos )
{
    if( rl_dist( guy.bub_pos(), pos ) > 1 || !get_map().inbounds( pos ) ) {
        return fail( _( "That is too far away." ) );
    }
    const int before = guy.get_moves();
    doors::close_door( get_map(), guy, pos );
    if( guy.get_moves() == before ) {
        return fail( _( "There is nothing you can close there." ) );
    }
    return action_result{};
}

action_result do_smash( npc &guy, const tripoint_bub_ms &target, const bool pulp_acid )
{
    map &here = get_map();
    tripoint_bub_ms smashp = target;
    if( rl_dist( guy.bub_pos().xy(), smashp.xy() ) > 1 || !here.inbounds( smashp ) ) {
        return fail( _( "That is too far away." ) );
    }
    bool smash_floor = false;
    if( smashp.z() < guy.bub_pos().z() ) {
        smashp.z() = guy.bub_pos().z();
        smash_floor = true;
    } else if( smashp.z() > guy.bub_pos().z() ) {
        return fail( _( "You can't reach that." ) );
    }
    // Corpses that would get back up are pulped first, like the player does.
    // Same test as the pulping activity itself, so it never starts on a
    // pile it would not touch (e.g. with revival disabled by a mod).
    bool has_acid = false;
    bool should_pulp = false;
    for( const item *it : here.i_at( smashp ) ) {
        if( !it->is_corpse() || it->damage() >= it->max_damage() ) {
            continue;
        }
        const mtype *corpse_type = it->get_mtype();
        if( !corpse_type->has_flag( MF_REVIVES ) && !corpse_type->zombify_into ) {
            continue;
        }
        if( corpse_type->bloodType()->has_acid ) {
            has_acid = true;
            if( !pulp_acid ) {
                continue;
            }
        }
        should_pulp = true;
    }
    if( should_pulp ) {
        guy.assign_activity( std::make_unique<player_activity>( activity_id( "ACT_PULP" ),
                             calendar::INDEFINITELY_LONG, 0 ) );
        guy.activity->placement = bub_to_abs( smashp );
        if( !pulp_acid ) {
            guy.activity->str_values.emplace_back( "auto_pulp_no_acid" );
        }
        return action_result{};
    }
    if( has_acid ) {
        // The player chose not to pulp an acid filled corpse.
        return fail( std::string() );
    }
    item &weapon = guy.primary_weapon();
    const int smashskill = guy.str_cur + weapon.damage_melee( DT_BASH );
    const int move_cost = !guy.is_armed() ? 80 : static_cast<int>( guy.attack_cost( weapon ) * 0.8 );
    const bash_params bash{
        .strength = smashskill,
        .silent = false,
        .destroy = false,
        .bash_floor = smash_floor,
        .roll = static_cast<float>( rng_float( 0, 1.0f ) ),
        .bashing_from_above = false,
        .do_recurse = true,
        .caused_by_player = true
    };
    if( !here.bash( smashp, bash ).did_bash ) {
        return fail( _( "There's nothing there to smash!" ) );
    }
    guy.mod_moves( -move_cost );
    if( smashskill < here.bash_resistance( smashp ) && one_in( 10 ) ) {
        guy.add_msg_if_player( m_neutral, _( "You don't seem to be damaging it." ) );
    }
    return action_result{};
}

// The ground item the client means: by its index in the pile, or, when the
// pile changed order, the same one of that type.
item *resolve_ground_item( const std::vector<item *> &ground, const int index,
                           const std::string &type, const int ordinal )
{
    if( index >= 0 && static_cast<size_t>( index ) < ground.size() &&
        ground[index]->typeId().str() == type ) {
        return ground[index];
    }
    item *it = nullptr;
    int seen = 0;
    for( item *candidate : ground ) {
        if( candidate->typeId().str() != type ) {
            continue;
        }
        if( it == nullptr || seen == ordinal ) {
            it = candidate;
        }
        if( seen == ordinal ) {
            break;
        }
        ++seen;
    }
    return it;
}

std::vector<item *> ground_items( const tripoint_bub_ms &pos )
{
    std::vector<item *> ground;
    for( item *it : get_map().i_at( pos ) ) {
        ground.push_back( it );
    }
    return ground;
}

// [[index, count, type, ordinal], ...] -> resolved (item, count) pairs.
// The cargo part of a vehicle at `pos`, or -1.
int cargo_part_at( const tripoint_bub_ms &pos, vehicle *&veh )
{
    veh = nullptr;
    const optional_vpart_position vp = get_map().veh_at( pos );
    if( !vp ) {
        return -1;
    }
    veh = &vp->vehicle();
    return veh->part_with_feature( vp->part_index(), "CARGO", false );
}

std::vector<item *> cargo_items( const tripoint_bub_ms &pos )
{
    std::vector<item *> cargo;
    vehicle *veh = nullptr;
    const int part = cargo_part_at( pos, veh );
    if( part >= 0 ) {
        for( item *it : veh->get_items( part ) ) {
            cargo.push_back( it );
        }
    }
    return cargo;
}

// Entries are [index, count, type, ordinal(, 1 if in a vehicle's cargo)].
std::vector<std::pair<item *, int>> resolve_ground_items( const tripoint_bub_ms &pos,
        const JsonArray &list )
{
    const std::vector<item *> ground = ground_items( pos );
    const std::vector<item *> cargo = cargo_items( pos );
    std::vector<std::pair<item *, int>> result;
    for( JsonArray entry : list ) {
        const int count = entry.get_int( 1 );
        const bool in_cargo = entry.size() > 4 && entry.get_int( 4 ) == 1;
        item *it = resolve_ground_item( in_cargo ? cargo : ground, entry.get_int( 0 ),
                                        entry.get_string( 2 ), entry.size() > 3 ? entry.get_int( 3 ) : -1 );
        if( it == nullptr || std::any_of( result.begin(), result.end(),
        [it]( const std::pair<item *, int> &r ) {
        return r.first == it;
    } ) ) {
            continue;
        }
        result.emplace_back( it, count );
    }
    return result;
}

std::vector<std::pair<item *, int>> resolve_carried_items( npc &guy, const JsonArray &list )
{
    std::vector<std::pair<item *, int>> result;
    for( JsonArray entry : list ) {
        const int index = entry.get_int( 0 );
        const int count = entry.get_int( 1 );
        const std::string type = entry.get_string( 2 );
        const int ordinal = entry.size() > 3 ? entry.get_int( 3 ) : -1;
        const std::string name = entry.size() > 4 ? entry.get_string( 4 ) : std::string();
        item *it = find_carried_item( guy, index, type, ordinal, name );
        if( it == nullptr ) {
            continue;
        }
        // The same item picked twice (two entries resolved to one): skip.
        if( std::any_of( result.begin(), result.end(), [it]( const std::pair<item *, int> &r ) {
        return r.first == it;
    } ) ) {
            continue;
        }
        result.emplace_back( it, count );
    }
    return result;
}

action_result do_pickup( npc &guy, const tripoint_bub_ms &pos, const JsonArray &list )
{
    map &here = get_map();
    if( rl_dist( guy.bub_pos(), pos ) > 1 || !here.inbounds( pos ) ) {
        return fail( _( "That is too far away." ) );
    }
    const auto targets = resolve_ground_items( pos, list );
    if( targets.empty() ) {
        return fail( _( "Those items are no longer there." ) );
    }
    int picked = 0;
    for( const auto &[it, count] : targets ) {
        const int qty = it->count_by_charges() ? std::clamp( count, 1, it->charges ) : 0;
        const units::volume vol = it->count_by_charges() ? it->volume() * qty / std::max( 1,
                                  it->charges ) : it->volume();
        const units::mass wgt = it->count_by_charges() ? it->weight() * qty / std::max( 1,
                                it->charges ) : it->weight();
        if( !guy.can_pick_volume( vol ) ) {
            guy.add_msg_if_player( m_bad, _( "There's no room in your inventory for the %s." ),
                                   it->tname() );
            continue;
        }
        if( !guy.can_pick_weight( wgt, false ) ) {
            guy.add_msg_if_player( m_bad, _( "The %s is too heavy!" ), it->tname() );
            continue;
        }
        const std::string name = it->tname( qty > 0 ? qty : 1 );
        detached_ptr<item> taken;
        if( qty > 0 && qty < it->charges ) {
            taken = it->split( qty );
        } else {
            vehicle *veh = nullptr;
            const int part = cargo_part_at( pos, veh );
            const std::vector<item *> cargo = cargo_items( pos );
            if( part >= 0 && std::find( cargo.begin(), cargo.end(), it ) != cargo.end() ) {
                taken = veh->remove_item( part, it );
            } else {
                taken = here.i_rem( pos, it );
            }
        }
        if( !taken ) {
            continue;
        }
        guy.i_add( std::move( taken ) );
        guy.add_msg_if_player( _( "You pick up %s." ), name );
        ++picked;
    }
    if( picked == 0 ) {
        return fail( std::string() );
    }
    guy.mod_moves( -std::min( 400, 100 + 25 * picked ) );
    return action_result{};
}

detached_ptr<item> take_from_character( npc &guy, item &it, int count )
{
    if( guy.is_wielding( it ) ) {
        return guy.remove_primary_weapon();
    }
    if( guy.is_worn( it ) ) {
        std::vector<detached_ptr<item>> removed;
        if( !guy.takeoff( it, &removed ) || removed.empty() ) {
            return detached_ptr<item>();
        }
        detached_ptr<item> first = std::move( removed.front() );
        for( size_t i = 1; i < removed.size(); ++i ) {
            guy.i_add_or_drop( std::move( removed[i] ) );
        }
        return first;
    }
    if( it.count_by_charges() && count > 0 && count < it.charges ) {
        return it.split( count );
    }
    return guy.remove_item( it );
}

action_result do_drop( npc &guy, const JsonArray &list, const std::optional<tripoint_bub_ms> &where )
{
    map &here = get_map();
    const tripoint_bub_ms dest = where.value_or( guy.bub_pos() );
    if( rl_dist( guy.bub_pos(), dest ) > 1 || !here.inbounds( dest ) ) {
        return fail( _( "That is too far away." ) );
    }
    if( !here.can_put_items_ter_furn( dest ) ) {
        return fail( _( "You can't place items there!" ) );
    }
    const auto targets = resolve_carried_items( guy, list );
    if( targets.empty() ) {
        return fail( _( "You no longer have that." ) );
    }
    int dropped = 0;
    for( const auto &[it, count] : targets ) {
        const std::string name = it->tname( it->count_by_charges() && count > 0 ? count : 1 );
        detached_ptr<item> d = take_from_character( guy, *it, count );
        if( !d ) {
            continue;
        }
        here.add_item_or_charges( dest, std::move( d ) );
        guy.add_msg_if_player( _( "You drop %s." ), name );
        ++dropped;
    }
    if( dropped == 0 ) {
        return fail( std::string() );
    }
    guy.mod_moves( -std::min( 400, 50 + 25 * dropped ) );
    return action_result{};
}

item *carried_item( npc &guy, const JsonObject &jo )
{
    return find_carried_item( guy, jo.get_int( "idx", -1 ), jo.get_string( "type", "" ),
                              jo.get_int( "ord", -1 ), jo.get_string( "name", "" ) );
}

action_result do_item_action( npc &guy, const std::string &what, const JsonObject &jo )
{
    if( what == "unwield" ) {
        if( !guy.is_armed() ) {
            return fail( _( "You are already empty handed." ) );
        }
        guy.wield( null_item_reference() );
        guy.add_msg_if_player( _( "You put away your weapon." ) );
        return action_result{};
    }
    if( what == "wield" && jo.has_int( "gidx" ) ) {
        // An item lying next to the player.
        const tripoint_bub_ms pos = read_bub( jo );
        if( rl_dist( guy.bub_pos(), pos ) > 1 || !get_map().inbounds( pos ) ) {
            return fail( _( "That is too far away." ) );
        }
        item *const found = resolve_ground_item( ground_items( pos ), jo.get_int( "gidx" ),
                            jo.get_string( "type", "" ), jo.get_int( "ord", -1 ) );
        if( found == nullptr ) {
            return fail( _( "That is no longer there." ) );
        }
        item &target = *found;
        const ret_val<bool> can = guy.can_wield( target );
        if( !can.success() ) {
            return fail( can.str() );
        }
        const std::string name = target.tname();
        guy.wield( target );
        guy.add_msg_if_player( _( "You wield your %s." ), name );
        return action_result{};
    }
    item *it = carried_item( guy, jo );
    if( it == nullptr ) {
        return fail( _( "You no longer have that." ) );
    }
    if( what == "wield" ) {
        if( guy.is_wielding( *it ) ) {
            guy.wield( null_item_reference() );
            guy.add_msg_if_player( _( "You put away your weapon." ) );
            return action_result{};
        }
        const ret_val<bool> can = guy.can_wield( *it );
        if( !can.success() ) {
            return fail( can.str() );
        }
        const std::string name = it->tname();
        if( guy.is_worn( *it ) ) {
            return fail( _( "Take it off first." ) );
        }
        guy.wield( *it );
        guy.add_msg_if_player( _( "You wield your %s." ), name );
        return action_result{};
    }
    if( what == "wear" ) {
        if( guy.is_worn( *it ) ) {
            return fail( _( "You are already wearing that." ) );
        }
        const ret_val<bool> can = guy.can_wear( *it );
        if( !can.success() ) {
            return fail( can.str() );
        }
        if( !guy.wear_possessed( *it, false ) ) {
            return fail( _( "You can't wear that." ) );
        }
        return action_result{};
    }
    if( what == "takeoff" ) {
        if( !guy.is_worn( *it ) ) {
            return fail( _( "You are not wearing that." ) );
        }
        const ret_val<bool> can = guy.can_takeoff( *it );
        if( !can.success() ) {
            return fail( can.str() );
        }
        std::vector<detached_ptr<item>> removed;
        if( !guy.takeoff( *it, &removed ) ) {
            return fail( _( "You can't take that off." ) );
        }
        for( detached_ptr<item> &r : removed ) {
            guy.i_add_or_drop( std::move( r ) );
        }
        return action_result{};
    }
    if( what == "eat" ) {
        const int before = guy.get_moves();
        guy.consume( *it );
        if( guy.get_moves() == before ) {
            guy.mod_moves( -100 );
        }
        return action_result{};
    }
    return fail( _( "That action is not available in co-op yet." ) );
}

// Hostiles the character sees within safe mode distance (like the
// player's safe mode).  `known` are the ones already there when waiting
// began: like in single player, only new ones interrupt.
bool hostile_in_view( npc &guy, std::string &what, const std::set<const monster *> *known = nullptr )
{
    const int proximity = get_option<int>( "SAFEMODEPROXIMITY" );
    const int range = proximity > 0 ? proximity : 60;
    for( monster &critter : g->all_monsters() ) {
        if( critter.is_dead() || rl_dist( critter.bub_pos(), guy.bub_pos() ) > range ) {
            continue;
        }
        if( known != nullptr && known->contains( &critter ) ) {
            continue;
        }
        if( critter.attitude_to( guy ) == Attitude::A_HOSTILE && guy.sees( critter ) ) {
            what = critter.get_name();
            return true;
        }
    }
    return false;
}

std::set<const monster *> visible_hostiles( npc &guy )
{
    std::set<const monster *> seen;
    for( monster &critter : g->all_monsters() ) {
        if( !critter.is_dead() && critter.attitude_to( guy ) == Attitude::A_HOSTILE && guy.sees( critter ) ) {
            seen.insert( &critter );
        }
    }
    return seen;
}

item *wielded_gun( npc &guy )
{
    if( !guy.is_armed() ) {
        return nullptr;
    }
    item &gun = guy.primary_weapon();
    return gun.is_gun() && !gun.is_gunmod() ? &gun : nullptr;
}

action_result do_fire( npc &guy, const JsonObject &jo )
{
    const tripoint_bub_ms target = read_bub( jo );
    item *gun = wielded_gun( guy );
    if( gun == nullptr ) {
        return fail( _( "You are not wielding a gun." ) );
    }
    if( !get_map().inbounds( target ) || target == guy.bub_pos() ) {
        return fail( std::string() );
    }
    // The fire mode the player chose in the targeting UI.
    if( jo.has_string( "mode" ) ) {
        gun->gun_set_mode( gun_mode_id( jo.get_string( "mode" ) ) );
    }
    gun_mode mode = gun->gun_current_mode();
    if( !mode ) {
        return fail( string_format( _( "Your %s can't be fired." ), gun->tname() ) );
    }
    if( !mode->ammo_sufficient( mode.qty > 1 ? mode.qty : 1 ) && !mode->ammo_sufficient() ) {
        return fail( string_format( _( "Your %s is empty." ), gun->tname() ) );
    }
    if( jo.has_member( "recoil" ) ) {
        // Aimed in the client's targeting UI: take its result and the time
        // it took (a long aim makes the character skip the following turns).
        guy.recoil = std::clamp( jo.get_float( "recoil" ), 0.0, static_cast<double>( MAX_RECOIL ) );
        guy.mod_moves( -std::clamp( jo.get_int( "aim_moves", 0 ), 0, 1000 ) );
    } else {
        guy.aim();
    }
    const int fired = ranged::fire_gun( guy, target, mode.qty, *mode, nullptr );
    if( fired == 0 ) {
        return fail( string_format( _( "You can't fire your %s." ), gun->tname() ) );
    }
    return action_result{};
}

action_result do_reload( npc &guy )
{
    item *gun = wielded_gun( guy );
    if( gun == nullptr ) {
        return fail( _( "You are not wielding a gun." ) );
    }
    if( !guy.can_reload( *gun ) ) {
        return fail( string_format( _( "Your %s is already fully loaded!" ), gun->tname() ) );
    }
    if( !reload_selection::prepare( guy, *gun ).selected ) {
        return fail( string_format( _( "You don't have any ammo for your %s." ), gun->tname() ) );
    }
    const int before = gun->ammo_remaining();
    guy.do_reload( *gun );
    if( gun->ammo_remaining() == before ) {
        return fail( string_format( _( "You can't reload your %s." ), gun->tname() ) );
    }
    guy.add_msg_if_player( _( "You reload your %s." ), gun->tname() );
    return action_result{};
}

action_result do_cycle_fire_mode( npc &guy )
{
    item *gun = wielded_gun( guy );
    if( gun == nullptr ) {
        return fail( _( "You are not wielding a gun." ) );
    }
    if( gun->gun_all_modes().size() < 2 ) {
        return fail( string_format( _( "Your %s has only one firing mode." ), gun->display_name() ) );
    }
    gun->gun_cycle_mode();
    guy.add_msg_if_player( _( "Firing mode: %s." ), gun->gun_current_mode().tname() );
    // Changing the mode takes no time.
    return action_result{};
}

// Crafting checks that would ask the crafter a question are done here instead:
// such a question would pop up on the host's screen.
action_result do_craft( npc &guy, const JsonObject &jo )
{
    const recipe_id id( jo.get_string( "recipe", "" ) );
    if( !id.is_valid() ) {
        return fail( _( "The host does not know that recipe." ) );
    }
    const recipe &making = id.obj();
    const int batch = std::clamp( jo.get_int( "batch", 1 ), 1, 1000 );
    if( making.result()->phase == LIQUID ) {
        return fail( _( "Crafting liquids is not available in co-op yet." ) );
    }
    if( !guy.can_make( &making, batch ) ) {
        return fail( _( "You can no longer make that craft!" ) + std::string( "\n" ) +
                     making.simple_requirements().list_missing() );
    }
    if( !guy.can_start_craft( &making, recipe_filter_flags::no_rotten, batch ) ) {
        return fail( _( "That craft would use rotten components." ) );
    }
    guy.make_craft_with_command( id, batch, jo.get_bool( "long", false ) );
    if( !guy.activity || !*guy.activity ) {
        return fail( _( "You can't start that craft." ) );
    }
    return action_result{};
}

// "Examine" (e) of furniture, terrain or a trap with a special function
// (the client handles plain item piles itself, through pickup).  Questions
// those functions ask show up on the host's screen.
action_result do_examine( npc &guy, const tripoint_bub_ms &pos )
{
    map &here = get_map();
    if( rl_dist( guy.bub_pos(), pos ) > 1 || !here.inbounds( pos ) ) {
        return fail( _( "That is too far away." ) );
    }
    if( monster *mon = g->critter_at<monster>( pos ) ) {
        // Like game::examine: the pet's menu, for this player.
        guy.add_msg_if_player( _( "There is a %s." ), mon->get_name() );
        if( mon->has_effect( efftype_id( "pet" ) ) && !guy.is_mounted() ) {
            monexamine::pet_menu( *mon );
        } else if( ( mon->has_flag( MF_RIDEABLE_MECH ) || mon->has_flag( MF_CARD_OVERRIDE ) ) &&
                   !mon->has_effect( efftype_id( "pet" ) ) &&
                   mon->attitude_to( guy ) != Attitude::A_HOSTILE ) {
            monexamine::mech_hack( *mon );
        } else if( mon->has_flag( MF_PAY_BOT ) ) {
            monexamine::pay_bot( *mon );
        } else if( mon->attitude_to( guy ) == Attitude::A_FRIENDLY && !guy.is_mounted() ) {
            monexamine::mfriend_menu( *mon );
        }
        return action_result{};
    }
    if( const optional_vpart_position vp = here.veh_at( pos ) ) {
        // The vehicle's menu, for this player; its questions go to them.
        vp->vehicle().interact_with( pos, vp->part_index(), &guy );
        return action_result{};
    }
    if( here.has_flag( "CONSOLE", pos ) ) {
        return fail( _( "Using computers is not available in co-op yet." ) );
    }
    const int before = guy.get_moves();
    const tripoint_bub_ms guy_pos = guy.bub_pos();
    if( here.has_furn( pos ) ) {
        here.furn( pos ).obj().examine( guy, pos );
    } else {
        here.ter( pos ).obj().examine( guy, pos );
    }
    if( guy.bub_pos() == guy_pos && !here.tr_at( pos ).is_null() ) {
        iexamine::trap( guy, pos );
    }
    if( guy.get_moves() == before && guy.bub_pos() == guy_pos && !( guy.activity && *guy.activity ) ) {
        guy.mod_moves( -50 );
    }
    return action_result{};
}

// The item an action is about: carried (idx/ord/name), or lying next to
// the player (x/y/z plus gidx/ord within that pile).
item *find_action_item( npc &guy, const JsonObject &jo, bool &on_ground )
{
    on_ground = jo.has_int( "gidx" );
    if( on_ground ) {
        const tripoint_bub_ms pos = read_bub( jo );
        if( rl_dist( guy.bub_pos(), pos ) > 1 || !get_map().inbounds( pos ) ) {
            return nullptr;
        }
        return resolve_ground_item( ground_items( pos ), jo.get_int( "gidx" ),
                                    jo.get_string( "type", "" ), jo.get_int( "ord", -1 ) );
    }
    return find_carried_item( guy, jo.get_int( "idx", -1 ), jo.get_string( "type", "" ),
                              jo.get_int( "ord", -1 ), jo.get_string( "name", "" ) );
}

// "Read" (R).  NPCs read only to learn a skill, so books just for fun
// can't be read this way yet.
action_result do_read( npc &guy, const JsonObject &jo )
{
    bool on_ground = false;
    item *book = find_action_item( guy, jo, on_ground );
    if( book == nullptr ) {
        return fail( _( "You no longer have that." ) );
    }
    // npc::can_read only allows books that teach a skill; a player may also
    // read for fun, so check like the player's own reading does.
    if( !book->is_book() ) {
        return fail( string_format( _( "Your %s is not good reading material." ), book->tname() ) );
    }
    const auto &reading = book->type->book;
    const skill_id &skill = reading->skill;
    if( reading->intel > 0 && guy.has_trait( trait_id( "ILLITERATE" ) ) ) {
        return fail( _( "You're illiterate!" ) );
    }
    if( guy.has_trait( trait_id( "HYPEROPIC" ) ) && !guy.worn_with_flag( flag_id( "FIX_FARSIGHT" ) ) &&
        !guy.has_effect( efftype_id( "contacts" ) ) && !guy.has_bionic( bionic_id( "bio_eye_optic" ) ) ) {
        return fail( _( "Your eyes won't focus without reading glasses." ) );
    }
    if( !character_funcs::can_see_fine_details( guy ) ) {
        return fail( _( "It's too dark to read!" ) );
    }
    if( skill && guy.get_skill_level( skill ) < reading->req ) {
        return fail( string_format( _( "You need %s %d to understand the jargon!" ), skill.obj().name(),
                                    reading->req ) );
    }
    const bool learns = skill && guy.get_skill_level( skill ) < reading->level;
    const bool fun = character_funcs::get_book_fun_for( guy, *book ) > 0;
    if( !learns && !fun && skill ) {
        return fail( _( "You won't learn anything from this book." ) );
    }
    if( !fun && !guy.has_morale_to_read() ) {
        return fail( _( "What's the point of studying?  (Your morale is too low!)" ) );
    }
    guy.start_read( *book, &guy );
    guy.add_msg_if_player( m_info, _( "Now reading %s." ), book->type_name() );
    return action_result{};
}

// "Disassemble" ('('), following crafting's prompt_disassemble_single (which
// only takes the avatar).  Its questions go to the player through the remote
// prompts, since this runs inside their action.
action_result do_disassemble( npc &guy, const JsonObject &jo )
{
    bool on_ground = false;
    item *obj = find_action_item( guy, jo, on_ground );
    if( obj == nullptr ) {
        return fail( _( "You no longer have that." ) );
    }
    const ret_val<bool> can = crafting::can_disassemble( guy, *obj, guy.crafting_inventory() );
    if( !can.success() ) {
        return fail( can.str() );
    }
    const recipe &r = recipe_dictionary::get_uncraft( obj->typeId() );
    const int batch_size = r.disassembly_batch_size();
    if( get_option<bool>( "QUERY_DISASSEMBLE" ) ) {
        std::string msg = string_format( _( "Disassembling the %s may yield:\n" ), obj->tname() );
        for( const item_comp &component : obj->get_uncraft_components() ) {
            msg += "- " + component.to_string() + "\n";
        }
        if( batch_size != 1 ) {
            msg += string_format( _( "(per batch of %d)\n" ), batch_size );
        }
        msg += "\n";
        msg += _( "Really disassemble?\n" );
        if( !query_yn( msg ) ) {
            return fail( _( "Never mind." ) );
        }
    }
    int batches = 1;
    if( obj->count_by_charges() ) {
        const int max_batches = obj->charges / std::max( 1, batch_size );
        if( max_batches == 0 ) {
            return fail( string_format( _( "You need at least %d to disassemble that." ), batch_size ) );
        }
        if( max_batches > 1 ) {
            string_input_popup popup_input;
            popup_input.title( string_format( _( "Disassemble how many batches of %s [MAX: %d]: " ),
                                              obj->type_name( 1 ), max_batches ) )
            .width( 20 )
            .only_digits( true );
            const int wanted = popup_input.query_int();
            if( popup_input.canceled() || wanted <= 0 ) {
                return fail( _( "Never mind." ) );
            }
            batches = std::min( wanted, max_batches );
        } else {
            batches = max_batches;
        }
    }
    std::vector<iuse_location> targets;
    targets.emplace_back( *obj, batches );
    guy.assign_activity( std::make_unique<player_activity>( std::make_unique<disassemble_activity_actor>
                         ( std::move( targets ), guy.abs_pos(), false ) ) );
    return action_result{};
}

// "Control vehicle" (^): take the wheel, or open the controls menu.
action_result do_control_vehicle( npc &guy )
{
    const optional_vpart_position vp = get_map().veh_at( guy.bub_pos() );
    if( !vp ) {
        guy.controlling_vehicle = false;
        return fail( _( "You aren't in a vehicle." ) );
    }
    vehicle &veh = vp->vehicle();
    const bool has_controls = veh.part_with_feature( vp->part_index(), "CONTROLS", true ) >= 0;
    if( !guy.controlling_vehicle && has_controls && veh.engine_on ) {
        if( !veh.interact_vehicle_locked() ) {
            return action_result{};
        }
        guy.controlling_vehicle = true;
        guy.add_msg_if_player( _( "You take control of the %s." ), veh.name );
        return action_result{};
    }
    veh.use_controls( guy.bub_pos(), &guy );
    return action_result{};
}

// Steering while at the wheel: x turns, y accelerates or brakes.
action_result do_drive( npc &guy, const JsonObject &jo )
{
    const optional_vpart_position vp = get_map().veh_at( guy.bub_pos() );
    if( !vp || !guy.controlling_vehicle ) {
        guy.controlling_vehicle = false;
        return fail( _( "You aren't driving." ) );
    }
    vehicle &veh = vp->vehicle();
    if( veh.part_with_feature( vp->part_index(), "CONTROLS", true ) < 0 &&
        veh.part_with_feature( vp->part_index(), "CONTROL_ANIMAL", true ) < 0 ) {
        guy.controlling_vehicle = false;
        return fail( _( "You can't drive the vehicle from here.  You need controls!" ) );
    }
    const tripoint_rel_veh d( std::clamp( jo.get_int( "dx", 0 ), -1, 1 ),
                              std::clamp( jo.get_int( "dy", 0 ), -1, 1 ), 0 );
    const int before = guy.get_moves();
    veh.pldrive( guy, d );
    if( guy.get_moves() == before ) {
        guy.mod_moves( -100 );
    }
    return action_result{};
}

// Throwing an item the player carries at the target they picked.
action_result do_throw( npc &guy, const JsonObject &jo )
{
    bool on_ground = false;
    item *it = find_action_item( guy, jo, on_ground );
    if( it == nullptr || on_ground ) {
        return fail( _( "You don't have that item." ) );
    }
    const tripoint_bub_ms target = abs_to_bub( tripoint_abs_ms( jo.get_int( "tx" ), jo.get_int( "ty" ),
                                   jo.get_int( "tz" ) ) );
    if( !get_map().inbounds( target ) || target == guy.bub_pos() ) {
        return fail( std::string() );
    }
    if( guy.throw_range( *it ) <= 0 ) {
        return fail( _( "That is too heavy to throw." ) );
    }
    if( guy.is_wielding( *it ) && it->has_flag( flag_id( "NO_UNWIELD" ) ) ) {
        return fail( _( "That's part of your body, you can't throw that!" ) );
    }
    if( guy.is_worn( *it ) ) {
        const ret_val<bool> ret = guy.can_takeoff( *it );
        if( !ret.success() ) {
            return fail( ret.str() );
        }
    }
    if( it != &guy.primary_weapon() ) {
        guy.mod_moves( -guy.item_handling_cost( *it, true, INVENTORY_HANDLING_PENALTY / 2 ) );
    }
    detached_ptr<item> thrown = it->split( 1 );
    if( !thrown ) {
        thrown = it->detach();
    }
    ranged::throw_item( guy, target, std::move( thrown ), std::nullopt );
    return action_result{};
}

// Reach attack (spear, whip...) at the target the player picked.
action_result do_reach( npc &guy, const tripoint_bub_ms &target )
{
    if( !get_map().inbounds( target ) || target == guy.bub_pos() ) {
        return fail( std::string() );
    }
    if( !guy.is_armed() || guy.primary_weapon().reach_range( guy ) < rl_dist( guy.bub_pos(), target ) ) {
        return fail( _( "That is out of your reach." ) );
    }
    guy.reach_attack( target );
    return action_result{};
}

// Tab, like avatar_action::autoattack: hit the most dangerous hostile in
// reach (melee or reach weapon), or wait a turn when there is none.
action_result do_autoattack( npc &guy )
{
    const int reach = guy.is_armed() ? guy.primary_weapon().reach_range( guy ) : 1;
    std::vector<Creature *> critters = ranged::targetable_creatures( guy, reach );
    critters.erase( std::remove_if( critters.begin(), critters.end(), [&guy]( const Creature * c ) {
        if( c == &guy || ( c->is_npc() && is_proxy( *c->as_npc() ) ) ) {
            return true;
        }
        if( c->is_monster() ) {
            return c->as_monster()->attitude_to( guy ) != Attitude::A_HOSTILE;
        }
        return !c->is_npc() || !c->as_npc()->is_enemy();
    } ), critters.end() );
    if( critters.empty() ) {
        guy.add_msg_if_player( m_info, _( "No hostile creature in reach.  Waiting a turn." ) );
        character_funcs::do_pause( guy );
        return action_result{};
    }
    const auto rate = []( const Creature * c ) -> float {
        if( c->is_monster() ) {
            return c->as_monster()->type->difficulty;
        }
        return 0.0f;
    };
    Creature &best = **std::max_element( critters.begin(), critters.end(),
    [&rate]( const Creature * l, const Creature * r ) {
        return rate( l ) < rate( r );
    } );
    const tripoint_rel_ms diff = best.bub_pos() - guy.bub_pos();
    if( std::abs( diff.x() ) <= 1 && std::abs( diff.y() ) <= 1 && diff.z() == 0 ) {
        guy.melee_attack( best, true );
    } else {
        guy.reach_attack( best.bub_pos() );
    }
    return action_result{};
}

// "Jump" over an adjacent obstacle.
action_result do_jump( npc &guy, const tripoint_bub_ms &pos )
{
    if( rl_dist( guy.bub_pos(), pos ) != 1 ) {
        return fail( _( "That is too far away." ) );
    }
    if( !iexamine::can_start_jump_over_tile( guy ) || !iexamine::can_jump_over_tile( guy, pos ) ) {
        return fail( _( "You can't jump across there." ) );
    }
    iexamine::jump_over_tile( guy, pos );
    return action_result{};
}

// Movement mode: "cycle" (like avatar::cycle_move_mode) or an explicit mode.
action_result do_move_mode( npc &guy, const JsonObject &jo )
{
    if( jo.get_bool( "cycle", false ) ) {
        int mode = ( static_cast<int>( guy.get_movement_mode() ) + 1 ) % CMM_COUNT;
        guy.set_movement_mode( static_cast<character_movemode>( mode ) );
        // A mode that can't be used right now is skipped.
        if( !guy.movement_mode_is( static_cast<character_movemode>( mode ) ) ) {
            mode = ( mode + 1 ) % CMM_COUNT;
            guy.set_movement_mode( static_cast<character_movemode>( mode ) );
        }
        return action_result{};
    }
    const int mode = jo.get_int( "mode", CMM_WALK );
    if( mode < CMM_WALK || mode >= CMM_COUNT ) {
        return fail( std::string() );
    }
    guy.set_movement_mode( static_cast<character_movemode>( mode ) );
    return action_result{};
}

// "Butcher" (B) the given corpses on the player's tile.
action_result do_butcher( npc &guy, const JsonObject &jo )
{
    static const std::set<std::string> kinds = {
        "ACT_BUTCHER", "ACT_BUTCHER_FULL", "ACT_FIELD_DRESS", "ACT_SKIN", "ACT_BLEED",
        "ACT_QUARTER", "ACT_DISMEMBER", "ACT_DISSECT"
    };
    const std::string kind = jo.get_string( "kind", "" );
    if( !kinds.contains( kind ) ) {
        return fail( std::string() );
    }
    std::vector<item *> targets;
    for( const auto &entry : resolve_ground_items( guy.bub_pos(), jo.get_array( "items" ) ) ) {
        if( entry.first->is_corpse() ) {
            targets.push_back( entry.first );
        }
    }
    if( targets.empty() ) {
        return fail( _( "There are no corpses here to butcher." ) );
    }
    guy.assign_activity( activity_id( kind ), 0, true );
    for( item *c : targets ) {
        guy.activity->targets.emplace_back( c );
    }
    return action_result{};
}

// "Use" (a), following avatar_funcs::use_item.  The client already picked
// the use method when the item has several.
action_result do_use( npc &guy, const JsonObject &jo )
{
    bool on_ground = false;
    item *it = find_action_item( guy, jo, on_ground );
    if( it == nullptr ) {
        return fail( on_ground ? _( "That is no longer there." ) : _( "You no longer have that." ) );
    }
    // Like the player: most things are picked up to be used.
    if( on_ground && !it->has_flag( flag_id( "ALLOWS_REMOTE_USE" ) ) ) {
        it = &it->obtain( guy );
    }
    const std::string method = jo.get_string( "method", "" );
    const int before = guy.get_moves();
    guy.last_item = it->typeId();
    if( !method.empty() ) {
        if( !it->type->use_methods.contains( method ) ) {
            return fail( string_format( _( "You can't do that with your %s." ), it->tname() ) );
        }
        const auto can = it->type->use_methods.at( method ).can_call( guy, *it, false, guy.bub_pos() );
        if( !can.success() ) {
            return fail( can.str() );
        }
        guy.invoke_item( it, method, guy.bub_pos() );
    } else if( it->type->has_use() ) {
        if( it->type->use_methods.size() != 1 ) {
            return fail( std::string() );
        }
        guy.invoke_item( it, it->type->use_methods.begin()->first, guy.bub_pos() );
    } else if( it->is_tool() ) {
        return fail( string_format( _( "You can't do anything interesting with your %s." ),
                                    it->tname() ) );
    } else if( !it->is_craft() && ( it->is_medication() || it->is_food() ||
                                    it->get_contained().is_food() || it->get_contained().is_medication() ) ) {
        guy.consume( *it );
    } else if( it->is_book() ) {
        return fail( _( "Use the read command for books." ) );
    } else if( it->has_flag( flag_id( "SPLINT" ) ) ) {
        const ret_val<bool> can = guy.can_wear( *it );
        if( !can.success() ) {
            return fail( can.str() );
        }
        guy.wear_possessed( *it, false );
    } else {
        return fail( string_format( _( "You can't do anything interesting with your %s." ),
                                    it->tname() ) );
    }
    guy.recalculate_enchantment_cache();
    guy.invalidate_crafting_inventory();
    if( guy.get_moves() == before ) {
        guy.mod_moves( -100 );
    }
    return action_result{};
}

// Casts a spell the character knows (C).  The player picked the spell, and
// the target if it needs one, on their own screen; the checks are those the
// game makes for the player before casting.
action_result do_cast( npc &guy, const JsonObject &jo )
{
    const spell_id sp_id( jo.get_string( "id", "" ) );
    if( !sp_id.is_valid() || !guy.magic->knows_spell( sp_id ) ) {
        return fail( _( "You don't know that spell." ) );
    }
    spell &sp = guy.magic->get_spell( sp_id );
    if( !( sp.has_flag( spell_flag::BRAWL ) || sp.has_flag( spell_flag::PHYSICAL ) ) &&
        guy.has_trait( trait_BRAWLER ) ) {
        return fail( _( "Pfft, that spell is for COWARDS, and a Brawler like you is no coward!" ) );
    }
    for( const trait_id &blocker : sp.get_blocker_muts() ) {
        if( guy.has_trait( blocker ) ) {
            return fail( string_format( _( "Your %s mutation prevents you from casting this spell!" ),
                                        blocker->name() ) );
        }
    }
    if( guy.is_armed() && !( sp.has_flag( spell_flag::NO_HANDS ) ||
                             sp.has_flag( spell_flag::PHYSICAL ) ) &&
        !guy.primary_weapon().has_flag( flag_MAGIC_FOCUS ) &&
        guy.primary_weapon().is_two_handed( guy ) ) {
        return fail( _( "You need at least one hand free to cast this spell!" ) );
    }
    if( !guy.magic->has_enough_energy( guy, sp ) ) {
        return fail( string_format( _( "You don't have enough %s to cast the spell." ),
                                    sp.energy_string() ) );
    }
    if( sp.energy_source() == hp_energy && !guy.has_quality( qual_CUT ) ) {
        return fail( _( "You cannot cast Blood Magic without a cutting implement." ) );
    }
    const auto hook_results = cata::run_hooks( "on_spell_try_cast", [&]( sol::table & params ) {
        params["char"] = static_cast<Character *>( &guy );
        params["spell"] = &sp;
    } );
    if( !hook_results.get_or( "allowed", true ) ) {
        return fail( std::string() );
    }
    if( sp.type->lua_callbacks && !sp.type->lua_callbacks->call_on_try_cast( guy, sp ) ) {
        return fail( std::string() );
    }

    auto act = std::make_unique<player_activity>( activity_id( "ACT_SPELLCASTING" ),
               sp.casting_time( guy ) );
    // Same values as a spell cast by the player: own level, may fail, costs energy.
    act->values = { -1, 0, 1 };
    act->name = sp.id().str();
    if( jo.has_int( "x" ) ) {
        // spellcasting_finish() takes it instead of asking for a target.
        act->coords.push_back( bub_to_abs( read_bub( jo ) ) );
    }
    guy.magic->casting_ignore = jo.get_bool( "ignore", false );
    if( guy.magic->casting_ignore ) {
        for( const distraction_type d : {
                 distraction_type::alert, distraction_type::noise, distraction_type::pain,
                 distraction_type::attacked, distraction_type::hostile_spotted_near,
                 distraction_type::hostile_spotted_far, distraction_type::talked_to,
                 distraction_type::asthma, distraction_type::weather_change
             } ) {
            act->ignore_distraction( d );
        }
    }
    guy.magic->set_last_cast_spell( sp.id() );
    guy.assign_activity( std::move( act ), false );
    return action_result{};
}

// Turns an installed bionic on or off.  It is addressed by its index among
// the character's bionics; the id is a cross-check.
action_result do_bionic( npc &guy, const JsonObject &jo )
{
    bionic_collection &bios = *guy.my_bionics;
    const int idx = jo.get_int( "idx", -1 );
    const bionic_id id( jo.get_string( "id", "" ) );
    bionic *bio = nullptr;
    if( idx >= 0 && idx < static_cast<int>( bios.size() ) && bios[idx].id == id ) {
        bio = &bios[idx];
    } else {
        const auto found = std::ranges::find( bios, id, &bionic::id );
        bio = found != bios.end() ? &*found : nullptr;
    }
    if( bio == nullptr ) {
        return fail( _( "You don't have that bionic." ) );
    }
    if( !bio->info().activated ) {
        return fail( string_format( _( "Your %s can't be activated." ), bio->info().name ) );
    }
    if( bio->powered ) {
        guy.deactivate_bionic( *bio );
    } else {
        guy.activate_bionic( *bio );
        if( bio->powered && bio->id == bio_cqb ) {
            // The game asks the player for a style right away.
            const std::string style = jo.get_string( "style", "" );
            if( !style.empty() && matype_id( style ).is_valid() ) {
                // The player's own menu already told them about it.
                guy.martial_arts_data->set_style( matype_id( style ), true );
            }
        }
    }
    return action_result{};
}

// Activates or deactivates a mutation (the mutations screen).
action_result do_mutation( npc &guy, const JsonObject &jo )
{
    const trait_id mut( jo.get_string( "id", "" ) );
    if( !mut.is_valid() || !guy.has_trait( mut ) ) {
        return fail( _( "You don't have that mutation." ) );
    }
    if( !mut->activated && !mut->transform ) {
        return fail( string_format( _( "You cannot activate %s!" ), mut->name() ) );
    }
    if( jo.get_bool( "on", true ) ) {
        guy.activate_mutation( mut );
    } else {
        guy.deactivate_mutation( mut );
    }
    return action_result{};
}

// Picks a martial arts style (the style menu).
action_result do_style( npc &guy, const JsonObject &jo )
{
    character_martial_arts &ma = *guy.martial_arts_data;
    if( jo.has_bool( "hands_free" ) ) {
        ma.keep_hands_free = jo.get_bool( "hands_free" );
    }
    if( !jo.has_string( "style" ) ) {
        return action_result{};
    }
    const matype_id style( jo.get_string( "style" ) );
    if( !style.is_valid() ) {
        return fail( std::string() );
    }
    // With the CQB bionic on, its styles may be picked without knowing them.
    const bool cqb = guy.has_active_bionic( bio_cqb );
    if( !ma.has_martialart( style ) && !cqb ) {
        return fail( _( "You don't know that style." ) );
    }
    // The player's own menu already told them about it.
    ma.set_style( style, cqb );
    return action_result{};
}

// Armor layers: the order the player arranged their clothes in on their
// screen, as [index in the current order, type, side, hidden].
action_result do_armor_layers( npc &guy, const JsonObject &jo )
{
    std::vector<item *> current;
    for( item *it : guy.worn ) {
        current.push_back( it );
    }
    std::vector<item *> wanted;
    std::vector<JsonArray> entries;
    for( JsonArray e : jo.get_array( "order" ) ) {
        const int index = e.get_int( 0 );
        if( index < 0 || index >= static_cast<int>( current.size() ) ||
            current[index]->typeId().str() != e.get_string( 1 ) ||
            std::ranges::find( wanted, current[index] ) != wanted.end() ) {
            return fail( _( "Your clothes changed meanwhile." ) );
        }
        wanted.push_back( current[index] );
        entries.push_back( e );
    }
    if( wanted.size() != current.size() ) {
        return fail( _( "Your clothes changed meanwhile." ) );
    }
    // Put each item in its place by swapping, as the armor screen does.
    for( size_t i = 0; i < wanted.size(); ++i ) {
        auto target = std::next( guy.worn.begin(), static_cast<std::ptrdiff_t>( i ) );
        if( *target == wanted[i] ) {
            continue;
        }
        auto from = std::find( std::next( target ), guy.worn.end(), wanted[i] );
        if( from != guy.worn.end() ) {
            std::swap( *target, *from );
        }
    }
    for( size_t i = 0; i < wanted.size(); ++i ) {
        item &it = *wanted[i];
        const auto wanted_side = static_cast<side>( entries[i].get_int( 2 ) );
        if( it.is_sided() && it.get_side() != wanted_side ) {
            guy.change_side( it, false );
        }
        if( entries[i].get_bool( 3 ) ) {
            it.set_flag( flag_id( "HIDDEN" ) );
        } else {
            it.unset_flag( flag_id( "HIDDEN" ) );
        }
    }
    guy.reset_encumbrance();
    return action_result{};
}

// Talks to an NPC (C, "Talk to whom?").  The conversation runs here with the
// player's character; its windows show up on the player's screen.
action_result do_talk( npc &guy, const JsonObject &jo )
{
    npc *who = g->find_npc( character_id( jo.get_int( "id", -1 ) ) );
    if( who == nullptr || who->is_dead() || is_proxy( *who ) ) {
        return fail( _( "There's nobody there." ) );
    }
    if( who->bub_pos().z() != guy.bub_pos().z() || !guy.sees( *who ) ||
        rl_dist( guy.bub_pos(), who->bub_pos() ) > SEEX * 2 ) {
        return fail( string_format( _( "%s can't hear you from there." ), who->get_name() ) );
    }
    who->talk_to_u();
    guy.mod_moves( -100 );
    return action_result{};
}

action_result execute( peer_t &p, npc &guy, message &m )
{
    JsonObject jo = m.object();
    const std::string a = jo.get_string( "a", "" );
    if( a == "move" ) {
        if( jo.get_bool( "auto", false ) ) {
            const tripoint_bub_ms dest = guy.bub_pos() + tripoint_rel_ms( jo.get_int( "dx", 0 ),
                                         jo.get_int( "dy", 0 ), 0 );
            if( auto_move_stopped( guy, p, dest ) ) {
                return fail( std::string() );
            }
        }
        return do_move( guy, jo.get_int( "dx", 0 ), jo.get_int( "dy", 0 ) );
    }
    if( a == "vmove" ) {
        return do_vertical_move( guy, jo.get_int( "dz", 0 ) );
    }
    if( a == "pause" ) {
        character_funcs::do_pause( guy );
        return action_result{};
    }
    if( a == "wait" ) {
        p.auto_wait_turns = std::clamp( jo.get_int( "turns", 1 ), 1, 24 * 60 * 60 );
        p.wait_until = jo.get_string( "until", "" );
        p.wait_weather = get_weather().weather_id;
        p.wait_known_hostiles = visible_hostiles( guy );
        guy.add_msg_if_player( m_info, _( "You start waiting." ) );
        return action_result{};
    }
    if( a == "open" ) {
        return do_open( guy, read_bub( jo ) );
    }
    if( a == "close" ) {
        return do_close( guy, read_bub( jo ) );
    }
    if( a == "smash" ) {
        return do_smash( guy, read_bub( jo ), jo.get_bool( "acid", false ) );
    }
    if( a == "pickup" ) {
        return do_pickup( guy, read_bub( jo ), jo.get_array( "items" ) );
    }
    if( a == "drop" ) {
        return do_drop( guy, jo.get_array( "items" ),
                        jo.has_int( "x" ) ? std::optional<tripoint_bub_ms>( read_bub( jo ) ) : std::nullopt );
    }
    if( a == "wield" || a == "unwield" || a == "wear" || a == "takeoff" || a == "eat" ) {
        return do_item_action( guy, a, jo );
    }
    if( a == "fire" ) {
        return do_fire( guy, jo );
    }
    if( a == "craft" ) {
        return do_craft( guy, jo );
    }
    if( a == "examine" ) {
        return do_examine( guy, read_bub( jo ) );
    }
    if( a == "grab" ) {
        return do_grab( guy, jo );
    }
    if( a == "haul" ) {
        return do_haul( guy );
    }
    if( a == "peek" ) {
        // The player peeked on their own screen; it takes time like game::peek.
        guy.mod_moves( -200 );
        return action_result{};
    }
    if( a == "read" ) {
        return do_read( guy, jo );
    }
    if( a == "butcher" ) {
        return do_butcher( guy, jo );
    }
    if( a == "use" ) {
        return do_use( guy, jo );
    }
    if( a == "construct" ) {
        const std::string err = try_start_construction( guy, construction_id( jo.get_string( "id", "" ) ),
                                read_bub( jo ) );
        return err.empty() ? action_result{} : fail( err );
    }
    if( a == "reload" ) {
        return do_reload( guy );
    }
    if( a == "fire_mode" ) {
        return do_cycle_fire_mode( guy );
    }
    if( a == "move_mode" ) {
        return do_move_mode( guy, jo );
    }
    if( a == "disassemble" ) {
        return do_disassemble( guy, jo );
    }
    if( a == "push" || a == "attack" ) {
        const tripoint_bub_ms pos = read_bub( jo );
        Creature *target = g->critter_at( pos );
        if( target == nullptr || target == &guy || rl_dist( guy.bub_pos(), pos ) != 1 ) {
            return fail( _( "There's nobody there." ) );
        }
        if( a == "attack" ) {
            guy.melee_attack( *target, true );
            if( npc *other = target->as_npc() ) {
                // Like the player's attack: it angers them even on a miss.
                other->on_attacked( guy );
            }
            return action_result{};
        }
        npc *other = target->as_npc();
        if( other == nullptr ) {
            return fail( std::string() );
        }
        const tripoint_bub_ms oldpos = other->bub_pos();
        other->move_away_from( guy.bub_pos(), true );
        guy.mod_moves( -20 );
        if( oldpos != other->bub_pos() ) {
            guy.add_msg_if_player( _( "%s moves out of the way." ), other->get_name() );
        } else {
            guy.add_msg_if_player( m_warning, _( "%s has nowhere to go!" ), other->get_name() );
        }
        return action_result{};
    }
    if( a == "aim" ) {
        // Aiming with the wielded gun used up this turn.
        if( wielded_gun( guy ) == nullptr ) {
            return fail( _( "You are not wielding a gun." ) );
        }
        guy.recoil = std::clamp( jo.get_float( "recoil", MAX_RECOIL ), 0.0, static_cast<double>( MAX_RECOIL ) );
        guy.set_moves( 0 );
        return action_result{};
    }
    if( a == "turret_fire" ) {
        const optional_vpart_position vp = get_map().veh_at( guy.bub_pos() );
        turret_data turret = vp ? vp->vehicle().turret_query( guy.abs_pos() ) : turret_data();
        if( !turret ) {
            return fail( _( "There is no turret here." ) );
        }
        if( turret.query() != turret_data::status::ready ) {
            return fail( string_format( _( "The %s can't fire right now." ), turret.name() ) );
        }
        const tripoint_bub_ms target = read_bub( jo );
        if( !get_map().inbounds( target ) || target == guy.bub_pos() ) {
            return fail( std::string() );
        }
        turret.fire( guy, bub_to_abs( target ) );
        return action_result{};
    }
    if( a == "shout" ) {
        guy.shout( jo.get_string( "text", "" ), false );
        return action_result{};
    }
    if( a == "autoattack" ) {
        return do_autoattack( guy );
    }
    if( a == "jump" ) {
        return do_jump( guy, read_bub( jo ) );
    }
    if( a == "throw" ) {
        return do_throw( guy, jo );
    }
    if( a == "reach" ) {
        return do_reach( guy, read_bub( jo ) );
    }
    if( a == "control_vehicle" ) {
        return do_control_vehicle( guy );
    }
    if( a == "drive" ) {
        return do_drive( guy, jo );
    }
    if( a == "cast" ) {
        return do_cast( guy, jo );
    }
    if( a == "bionic" ) {
        return do_bionic( guy, jo );
    }
    if( a == "mutation" ) {
        return do_mutation( guy, jo );
    }
    if( a == "style" ) {
        return do_style( guy, jo );
    }
    if( a == "talk" ) {
        return do_talk( guy, jo );
    }
    if( a == "armor_layers" ) {
        return do_armor_layers( guy, jo );
    }
    return fail( _( "That action is not available in co-op yet." ) );
}

// Within this distance a hostile interrupts whatever the character does, as
// it does for the player.
constexpr int dangerous_proximity = 5;

// What the game warns the player about while busy: hostiles coming into view
// or coming close.  Checked once per turn of the character.
void watch_for_distractions( npc &guy, peer_t &p )
{
    const std::vector<Creature *> hostiles = [&guy] {
        auto seen = guy.get_visible_creatures( MAX_VIEW_DISTANCE );
        std::erase_if( seen, [&guy]( const Creature * c )
        {
            return guy.attitude_to( *c ) != Attitude::A_HOSTILE;
        } );
        return seen;
    }();
    const int seen = static_cast<int>( hostiles.size() );
    if( seen > p.seen_hostiles ) {
        p.spotted = seen - p.seen_hostiles == 1 ?
                    string_format( _( "%s spotted!" ), hostiles.back()->get_name() ) :
                    std::string( _( "Monsters spotted!" ) );
    }
    const bool busy = guy.activity && *guy.activity && guy.activity->id() != activity_id( "ACT_AIM" );
    if( busy && !p.distraction ) {
        const auto close = std::ranges::find_if( hostiles, [&guy]( const Creature * c ) {
            return rl_dist( guy.bub_pos(), c->bub_pos() ) <= dangerous_proximity;
        } );
        if( close != hostiles.end() ) {
            p.distraction = std::make_pair( distraction_type::hostile_spotted_near,
                                            string_format( _( "The %s is dangerously close!" ), ( *close )->get_name() ) );
        } else if( seen > p.seen_hostiles ) {
            p.distraction = std::make_pair( distraction_type::hostile_spotted_far,
                                            seen - p.seen_hostiles == 1 ?
                                            string_format( _( "%s spotted!" ), hostiles.back()->get_name() ) :
                                            std::string( _( "Monsters spotted!" ) ) );
        }
    }
    p.seen_hostiles = seen;
}

// Asks the player whether their character should stop what it is doing, like
// the player is asked.  The game waits for the answer.  Returns true if the
// activity was stopped.
bool ask_about_distraction( npc &guy, const int peer_id )
{
    peer_t *p = find_peer( peer_id );
    if( p == nullptr || !p->distraction ) {
        return false;
    }
    const auto [type, text] = *p->distraction;
    p->distraction.reset();
    if( !guy.activity || !*guy.activity || guy.activity->is_distraction_ignored( type ) ) {
        return false;
    }
    int pick = -1;
    {
        prompt_scope questions( peer_id );
        pick = remote_choice( string_format( "%s %s", text, guy.activity->get_stop_phrase() ), {
            _( "Yes" ), _( "No" ), _( "Ignore" )
        }, {}, false );
    }
    if( pick == 0 ) {
        guy.cancel_activity();
        // Some activities don't end on cancel for an NPC; the player asked
        // to stop, so stop.
        if( guy.activity && *guy.activity ) {
            guy.activity = std::make_unique<player_activity>();
        }
        guy.backlog.clear();
        return true;
    }
    if( pick == 2 && guy.activity && *guy.activity ) {
        guy.activity->ignore_distraction( type );
        for( auto &activity : guy.backlog ) {
            activity->ignore_distraction( type );
        }
    }
    return false;
}

// Waits for the next action of player `peer_id`.  Returns false if the player
// left, or the host decided to skip the turn.
bool wait_for_action( int peer_id )
{
    const int64_t started = net::now_ms();
    int64_t last_turn_sent = started;
    std::unique_ptr<static_popup> notice;
    // The usual keys: F2 for options, and screens that take no time
    // (inventory, look around, map...) while the other player thinks.
    input_context ctxt( "DEFAULTMODE" );
    ctxt.register_action( action_ident( ACTION_COOP_PLAYERS ) );
    for( const action_id act : {
             ACTION_INVENTORY, ACTION_LOOK, ACTION_MAP, ACTION_MESSAGES, ACTION_PL_INFO, ACTION_COOP_CHAT,
             ACTION_PAUSE
         } ) {
        ctxt.register_action( action_ident( act ) );
    }
    while( true ) {
        pump();
        peer_t *p = find_peer( peer_id );
        if( p == nullptr || !p->joined ) {
            return false;
        }
        if( p->action ) {
            return true;
        }
        // Remind the client now and then: if it lost track of whose turn it
        // is, this gets both sides moving again instead of waiting forever.
        if( net::now_ms() - last_turn_sent > 5000 ) {
            send( peer_id, "{\"t\":\"turn\"}" );
            last_turn_sent = net::now_ms();
        }
        if( !notice && net::now_ms() - started > 400 ) {
            notice = std::make_unique<static_popup>();
            notice->on_top( true );
        }
        if( notice ) {
            const int secs = static_cast<int>( ( net::now_ms() - started ) / 1000 );
            notice->message( _( "Waiting for %s to act… (%d s)\n%s for options" ),
                             p->name, secs, ctxt.get_desc( action_ident( ACTION_COOP_PLAYERS ) ) );
        }
        ui_manager::redraw();
        refresh_display();
        ctxt.set_timeout( 40 );
        const std::string action = ctxt.handle_input();
        // The host's own waiting or long action stops on the pause key, as in
        // single player (the other player's turns keep it running otherwise).
        if( action == action_ident( ACTION_PAUSE ) ) {
            avatar &host_char = get_avatar();
            if( host_char.activity && *host_char.activity ) {
                host_char.cancel_activity();
                add_msg( m_info, _( "You stop what you are doing." ) );
            }
        }
        if( action == action_ident( ACTION_INVENTORY ) ) {
            game_menus::inv::common( get_avatar() );
        } else if( action == action_ident( ACTION_LOOK ) ) {
            g->look_around();
        } else if( action == action_ident( ACTION_MAP ) ) {
            ui::omap::display();
        } else if( action == action_ident( ACTION_MESSAGES ) ) {
            Messages::display_messages();
        } else if( action == action_ident( ACTION_PL_INFO ) ) {
            character_display::disp_info( get_avatar() );
        } else if( action == action_ident( ACTION_COOP_CHAT ) ) {
            open_chat();
        }
        if( action == action_ident( ACTION_COOP_PLAYERS ) ) {
            uilist menu;
            menu.text = string_format( _( "%s has not acted yet." ), p->name );
            menu.addentry( 0, true, 'w', _( "Keep waiting" ) );
            menu.addentry( 1, true, 's', _( "Skip their turn" ) );
            menu.addentry( 4, true, 'd', _( "Don't wait for them (until they act)" ) );
            menu.addentry( 2, true, 'c', _( "Chat" ) );
            menu.addentry( 3, true, 'k', _( "Disconnect them" ) );
            menu.query();
            if( menu.ret == 1 ) {
                return false;
            } else if( menu.ret == 4 ) {
                p->dont_wait = true;
                send_to_player( *p, m_warning,
                                _( "The host stopped waiting for you.  Act whenever you are ready." ) );
                return false;
            } else if( menu.ret == 2 ) {
                open_chat();
            } else if( menu.ret == 3 ) {
                disconnect_peer( *p, _( "The host disconnected you." ) );
                return false;
            }
        }
    }
}

// Runs one action the player sent for their character and tells them how it
// went.
void run_action( peer_t &p, npc &guy, const std::string &line )
{
    message m( line );
    const int peer_id = p.id;
    int seq = 0;
    const int moves_before_action = guy.get_moves();
    action_result result;
    try {
        JsonObject jo = m.object();
        seq = jo.get_int( "seq", 0 );
        prompt_scope questions( peer_id );
        result = execute( p, guy, m );
    } catch( const std::exception &e ) {
        log( std::string( "action failed: " ) + e.what() );
        result = fail( _( "The host could not process that action." ) );
    }
    // What came into view so far is known to the player once they acted.
    p.spotted.reset();
    if( !result.ok && !result.msg.empty() ) {
        send_to_player( p, m_info, result.msg );
    }
    broadcast( sync_level::normal );
    // Moves the action took: the sidebar's move counter, like the player's.
    send( peer_id, string_format( "{\"t\":\"ack\",\"seq\":%d,\"ok\":%s,\"spent\":%d}", seq,
                                    result.ok ? "true" : "false", std::max( 0, moves_before_action - guy.get_moves() ) ) );
}

} // namespace

// ============================================================================
// Public host API
// ============================================================================

bool listening()
{
    return net::host_listening();
}

bool start_listening( std::string &err )
{
    peers.clear();
    net::clear_events();
    if( !net::host_listen( settings().port, err ) ) {
        log( "listen failed: " + err );
        return false;
    }
    log( string_format( "hosting world '%s' on port %d, build %s", world_name(), settings().port,
                        protocol_version_string() ) );
    // Players that were online when the world was saved are offline now.
    for( npc &guy : g->all_npcs() ) {
        if( is_proxy( guy ) ) {
            guy.add_effect( effect_npc_suspend, time_duration::from_turns( calendar::INDEFINITELY_LONG ) );
        }
    }
    return true;
}

void stop( const std::string &reason )
{
    // The players' characters are kept however the host's game ends.
    snapshot_online_players();
    for( auto &[id, p] : peers ) {
        send( id, "{\"t\":\"bye\",\"msg\":" + json_quote( reason ) + "}" );
        if( p.joined ) {
            park_proxy( p );
        }
    }
    peers.clear();
    net::host_shutdown();
    net::clear_events();
    log( "host stopped: " + reason );
}

void handle_opened( int peer, const std::string &address )
{
    peer_t &p = peers[peer];
    p.id = peer;
    p.address = address;
    log( string_format( "peer %d connected from %s", peer, address ) );
}

void handle_closed( int peer, const std::string &reason )
{
    peer_t *p = find_peer( peer );
    if( p == nullptr ) {
        return;
    }
    log( string_format( "peer %d (%s) closed: %s", peer, p->address, reason ) );
    if( p->joined ) {
        add_msg( m_warning, _( "Co-op: %s disconnected (%s)." ), p->name, reason );
        park_proxy( *p );
        const std::string name = p->name;
        for( auto &[id, other] : peers ) {
            if( other.joined && id != peer ) {
                send_to_player( other, m_warning, string_format( _( "%s left the game." ), name ) );
            }
        }
    }
    peers.erase( peer );
}

void handle_line( int peer, const std::string &line )
{
    peer_t *p = find_peer( peer );
    if( p == nullptr ) {
        return;
    }
    message m( line );
    if( !m.valid() ) {
        return;
    }
    const std::string &t = m.type();
    if( t == "probe" ) {
        handle_probe( *p, m );
    } else if( t == "join" ) {
        handle_join( *p, m );
    } else if( !p->joined ) {
        // Anything else before joining is ignored.
    } else if( t == "act" ) {
        if( !p->action ) {
            p->action = line;
        }
        p->dont_wait = false;
    } else if( t == "chat" ) {
        JsonObject jo = m.object();
        broadcast_chat( p->name, jo.get_string( "text", "" ) );
    } else if( t == "answer" ) {
        JsonObject jo = m.object();
        answers[jo.get_int( "id", 0 )] = line;
    } else if( t == "stop_wait" ) {
        npc *proxy = proxy_of( *p );
        if( p->auto_wait_turns > 0 ) {
            p->auto_wait_turns = 0;
            if( proxy != nullptr ) {
                proxy->add_msg_if_player( m_info, _( "You stop waiting." ) );
            }
        }
        if( proxy != nullptr && proxy->activity && *proxy->activity ) {
            proxy->cancel_activity();
            // Some activities don't end on cancel for an NPC; the player
            // asked to stop, so stop.
            if( proxy->activity && *proxy->activity ) {
                proxy->activity = std::make_unique<player_activity>();
            }
            proxy->backlog.clear();
        }
    } else if( t == "need" ) {
        JsonObject jo = m.object();
        for( JsonArray e : jo.get_array( "sm" ) ) {
            const tripoint_abs_sm sp( e.get_int( 0 ), e.get_int( 1 ), e.get_int( 2 ) );
            if( get_map().inbounds( sp ) ) {
                p->sm_sent.erase( sp );
                p->forced.insert( sp );
            }
        }
    } else if( t == "resync" ) {
        p->full_sync = true;
        p->sm_sent.clear();
        p->synced_z.clear();
        p->omt_sent.clear();
        p->omt_center.reset();
    } else if( t == "quit" ) {
        net::host_close_peer( peer );
    }
}

void broadcast_chat( const std::string &from, const std::string &text )
{
    if( text.empty() ) {
        return;
    }
    const std::string line = string_format( "%s: %s", from, text );
    add_msg( m_info, "<color_light_cyan>%s</color>", line );
    for( auto &[id, p] : peers ) {
        if( p.joined ) {
            send( id, "{\"t\":\"chat\",\"text\":" + json_quote( line ) + "}" );
        }
    }
}

bool perform_action( const int peer, const std::string &line )
{
    peer_t *p = find_peer( peer );
    npc *guy = p != nullptr ? proxy_of( *p ) : nullptr;
    if( guy == nullptr ) {
        return false;
    }
    run_action( *p, *guy, line );
    return true;
}

int joined_count()
{
    int n = 0;
    for( const auto &[id, p] : peers ) {
        n += p.joined ? 1 : 0;
    }
    return n;
}

std::vector<std::string> joined_names()
{
    std::vector<std::string> names;
    for( const auto &[id, p] : peers ) {
        if( p.joined ) {
            names.push_back( p.name );
        }
    }
    return names;
}

void kick_menu()
{
    uilist menu;
    menu.text = _( "Disconnect which player?" );
    std::vector<int> ids;
    for( const auto &[id, p] : peers ) {
        if( p.joined ) {
            menu.addentry( static_cast<int>( ids.size() ), true, MENU_AUTOASSIGN, p.name );
            ids.push_back( id );
        }
    }
    menu.query();
    if( menu.ret >= 0 && static_cast<size_t>( menu.ret ) < ids.size() ) {
        if( peer_t *p = find_peer( ids[menu.ret] ) ) {
            disconnect_peer( *p, _( "The host disconnected you." ) );
        }
    }
}

std::string status_line()
{
    const std::vector<std::string> names = joined_names();
    if( names.empty() ) {
        return string_format( _( "Co-op: hosting on port %d, nobody connected yet." ), net::host_port() );
    }
    std::string list;
    for( const std::string &n : names ) {
        list += list.empty() ? "" : ", ";
        list += n;
    }
    return string_format( _( "Co-op: hosting on port %d.  Players (%d): %s" ), net::host_port(),
                          static_cast<int>( names.size() ), list );
}

} // namespace cata_mp::host

// ============================================================================
// Game hooks (namespace cata_mp)
// ============================================================================

namespace cata_mp
{

void host_after_player_action()
{
    if( !is_host() ) {
        return;
    }
    pump();
    host::broadcast( host::sync_level::light );
}

void host_end_of_turn()
{
    if( is_client() ) {
        return;
    }
    // The characters of players who left (or of a co-op world played alone)
    // are taken out of the world until their players come back.
    host::stash_offline_proxies();
    if( !is_host() ) {
        return;
    }
    pump();
    // Now and then a copy of everybody who plays, in case the host never
    // saves (or crashes).
    static int64_t last_snapshot = net::now_ms();
    if( net::now_ms() - last_snapshot > 60 * 1000 ) {
        last_snapshot = net::now_ms();
        host::snapshot_online_players();
    }
    // The other players have taken their turns: it is the host's again.
    if( host::acting_name != host::host_name() ) {
        host::acting_name = host::host_name();
        host::broadcast( host::sync_level::light );
    }
    // While time runs fast (waiting, long activities) a full broadcast every
    // turn would flood the clients.  Skipping some is safe: every broadcast
    // sends whatever differs from what each client last received.
    static int64_t last_full = 0;
    const int64_t now = net::now_ms();
    if( now - last_full < 100 ) {
        return;
    }
    last_full = now;
    host::broadcast( host::sync_level::full );
}

bool host_proxy_turn( npc &guy )
{
    if( !is_proxy( guy ) ) {
        return false;
    }
    if( !is_host() ) {
        // Single player session on a co-op world: the remote player is offline.
        guy.set_moves( 0 );
        return true;
    }
    host::peer_t *p = host::peer_of_proxy( guy.getID() );
    if( p == nullptr ) {
        guy.set_moves( 0 );
        return true;
    }
    const int peer_id = p->id;
    if( !host::in_safe_area( guy.bub_pos() ) ) {
        host::place_npc_near_host( guy );
        guy.add_msg_if_player( m_info, _( "You catch up with %s." ), get_avatar().get_name() );
    }
    // The player looks around for hidden traps each turn; the game only does
    // that for the avatar.
    if( guy.get_moves() > 0 ) {
        character_funcs::search_surroundings( guy );
        host::watch_for_distractions( guy, *p );
    }
    int guard = 0;
    while( !guy.is_dead() && guy.get_moves() > 0 && guard++ < 64 ) {
        p = host::find_peer( peer_id );
        if( p == nullptr || !p->joined ) {
            guy.set_moves( 0 );
            break;
        }
        // Something interrupts what the character is doing: its player
        // decides, and everybody waits for them.
        if( host::ask_about_distraction( guy, peer_id ) ) {
            continue;
        }
        // Long actions (dropping into a container, eating...) keep going on
        // their own, exactly like they do for the player character.
        if( guy.activity && *guy.activity ) {
            const int before = guy.get_moves();
            // Finishing an activity reverts an NPC to its previous attitude
            // and mission; a player's character must keep its own.
            const npc_attitude attitude = guy.get_attitude();
            const npc_mission mission = guy.mission;
            {
                host::prompt_scope questions( peer_id );
                guy.activity->do_turn( guy );
            }
            if( guy.get_attitude() != attitude ) {
                guy.set_attitude( attitude );
            }
            guy.mission = mission;
            if( guy.get_moves() == before ) {
                // An activity that makes no progress for the player's
                // character would hold their turn forever: drop it.
                if( ++p->stalled_activity_turns > 3 ) {
                    guy.activity = std::make_unique<player_activity>();
                    guy.backlog.clear();
                    p->stalled_activity_turns = 0;
                    continue;
                }
                guy.set_moves( 0 );
            } else {
                p->stalled_activity_turns = 0;
            }
            continue;
        }
        if( p->auto_wait_turns > 0 ) {
            std::string what;
            if( host::hostile_in_view( guy, what, &p->wait_known_hostiles ) ) {
                p->auto_wait_turns = 0;
                guy.add_msg_if_player( m_warning, _( "You stop waiting: you see a %s!" ), what );
                continue;
            }
            if( ( p->wait_until == "stamina" && guy.get_stamina() >= guy.get_stamina_max() ) ||
                ( p->wait_until == "weather" && get_weather().weather_id != p->wait_weather ) ) {
                p->auto_wait_turns = 1;
            }
            --p->auto_wait_turns;
            character_funcs::do_pause( guy );
            if( p->auto_wait_turns == 0 ) {
                guy.add_msg_if_player( m_info, _( "You finish waiting." ) );
            }
            continue;
        }
        if( !p->action && p->dont_wait ) {
            guy.set_moves( 0 );
            break;
        }
        if( !p->action ) {
            host::acting_name = p->name;
            host::broadcast( host::sync_level::normal );
            if( !p->turn_announced ) {
                host::send( peer_id, "{\"t\":\"turn\"}" );
                p->turn_announced = true;
            }
            if( !host::wait_for_action( peer_id ) ) {
                p = host::find_peer( peer_id );
                if( p != nullptr ) {
                    p->turn_announced = false;
                }
                guy.set_moves( 0 );
                break;
            }
            p = host::find_peer( peer_id );
            if( p == nullptr ) {
                break;
            }
        }
        const std::string line = *p->action;
        p->action.reset();
        p->turn_announced = false;
        host::run_action( *p, guy, line );
    }
    return true;
}

bool host_keep_proxy_in_bubble( npc &guy )
{
    if( !is_proxy( guy ) ) {
        return false;
    }
    // The host moved far enough to drop the proxy out of the reality bubble:
    // bring it along instead of unloading it.
    host::place_npc_near_host( guy );
    guy.add_msg_if_player( m_info, _( "You catch up with %s." ), get_avatar().get_name() );
    return true;
}

void host_proxy_died( const npc &guy )
{
    if( !is_host() ) {
        return;
    }
    host::peer_t *p = host::peer_of_proxy( guy.getID() );
    if( p == nullptr ) {
        return;
    }
    add_msg( m_bad, _( "Co-op: %s has died." ), p->name );
    // No older copy of the character may bring it back.
    host::load_stored_players();
    if( host::stored_players.erase( p->name ) > 0 ) {
        host::save_stored_players();
    }
    host::send( p->id, "{\"t\":\"died\"}" );
    p->joined = false;
    net::host_close_peer( p->id );
}

void host_died()
{
    if( is_host() ) {
        host::stop( _( "The host's game has ended." ) );
        set_role( role::none );
    }
}

void open_players_menu()
{
    if( is_client() ) {
        add_msg( m_info, _( "Only the host can manage players." ) );
        return;
    }
    if( !is_host() ) {
        add_msg( m_info, _( "Co-op chat is only available in a co-op session." ) );
        return;
    }
    std::vector<int> ids;
    uilist pick;
    pick.text = _( "Which player?" );
    for( auto &[id, p] : host::peers ) {
        if( p.joined ) {
            pick.addentry( static_cast<int>( ids.size() ), true, MENU_AUTOASSIGN, p.name );
            ids.push_back( id );
        }
    }
    if( ids.empty() ) {
        add_msg( m_info, _( "Co-op: nobody is connected." ) );
        return;
    }
    int peer_id = ids.front();
    if( ids.size() > 1 ) {
        pick.query();
        if( pick.ret < 0 || pick.ret >= static_cast<int>( ids.size() ) ) {
            return;
        }
        peer_id = ids[pick.ret];
    }
    host::peer_t *p = host::find_peer( peer_id );
    if( p == nullptr ) {
        return;
    }
    npc *proxy = host::proxy_of( *p );
    const bool busy = p->auto_wait_turns > 0 || ( proxy != nullptr && proxy->activity &&
                      *proxy->activity );
    uilist menu;
    menu.text = p->name;
    menu.addentry( 0, busy, 's', _( "Stop their waiting or activity" ) );
    menu.addentry( 1, true, 'd', p->dont_wait ? _( "Wait for their turns again" ) :
                   _( "Don't wait for them (until they act)" ) );
    menu.addentry( 2, true, 'c', _( "Chat" ) );
    menu.addentry( 3, true, 'k', _( "Disconnect them" ) );
    menu.query();
    switch( menu.ret ) {
        case 0:
            p->auto_wait_turns = 0;
            if( proxy != nullptr && proxy->activity && *proxy->activity ) {
                proxy->cancel_activity();
            }
            host::send_to_player( *p, m_warning, _( "The host interrupted what you were doing." ) );
            break;
        case 1:
            p->dont_wait = !p->dont_wait;
            if( p->dont_wait ) {
                host::send_to_player( *p, m_warning,
                                      _( "The host stopped waiting for you.  Act whenever you are ready." ) );
            }
            break;
        case 2:
            open_chat();
            break;
        case 3:
            host::disconnect_peer( *p, _( "The host disconnected you." ) );
            break;
        default:
            break;
    }
}

player &acting_player()
{
    player *actor = remote_actor();
    return actor != nullptr ? *actor : get_avatar();
}

std::vector<player *> party_members()
{
    std::vector<player *> ret = { &get_avatar() };
    if( is_host() ) {
        for( npc &guy : g->all_npcs() ) {
            if( is_proxy( guy ) && !guy.is_dead() ) {
                ret.push_back( &guy );
            }
        }
    }
    return ret;
}

void share_message( const int type, const std::string &text )
{
    if( !is_host() ) {
        return;
    }
    for( auto &[id, p] : host::peers ) {
        if( p.joined ) {
            host::send_to_player( p, static_cast<game_message_type>( type ), remove_color_tags( text ) );
        }
    }
}

void proxy_distraction( Character &who, const distraction_type type, const std::string &text )
{
    if( !is_host() || !is_proxy( who ) || !who.activity || !*who.activity ||
        who.activity->is_distraction_ignored( type ) ) {
        return;
    }
    host::peer_t *p = host::peer_of_proxy( who.getID() );
    if( p != nullptr && !p->distraction ) {
        p->distraction = std::make_pair( type, text );
    }
}

local_prompts_scope::local_prompts_scope() : saved_( host::prompt_peer )
{
    host::prompt_peer = -1;
}

local_prompts_scope::~local_prompts_scope()
{
    host::prompt_peer = saved_;
}

void share_shout( const Character &who, const std::string &text )
{
    if( !is_host() ) {
        return;
    }
    // About as far as a shout carries in the game.
    constexpr int shout_range = 60;
    for( auto &[id, p] : host::peers ) {
        const npc *guy = p.joined ? host::proxy_of( p ) : nullptr;
        if( guy == nullptr || guy->getID() == who.getID() || guy->is_deaf() ||
            rl_dist( guy->abs_pos(), who.abs_pos() ) > shout_range ) {
            continue;
        }
        host::send_to_player( p, m_info, remove_color_tags( text ) );
    }
}

namespace
{
// Sends `msg` to every remote player in game.
void send_to_all_players( const std::string &msg )
{
    for( auto &[id, p] : host::peers ) {
        if( p.joined && host::proxy_of( p ) != nullptr ) {
            host::send( p.id, msg );
        }
    }
}
} // namespace

void share_explosion( const tripoint_bub_ms &p, int radius, const nc_color &col,
                      const std::string &name )
{
    if( !is_host() || connected_players() == 0 ) {
        return;
    }
    const tripoint_abs_ms a = bub_to_abs( p );
    send_to_all_players( string_format(
                             "{\"t\":\"anim\",\"k\":\"exp\",\"x\":%d,\"y\":%d,\"z\":%d,\"r\":%d,\"c\":%s,\"n\":%s}",
                             a.x(), a.y(), a.z(), radius, json_quote( string_from_color( col ) ), json_quote( name ) ) );
}

void share_custom_explosion( const std::map<tripoint_bub_ms, nc_color> &area,
                             const std::string &name )
{
    if( !is_host() || connected_players() == 0 || area.empty() ) {
        return;
    }
    std::string pts;
    for( const auto &[p, col] : area ) {
        const tripoint_abs_ms a = bub_to_abs( p );
        pts += pts.empty() ? "" : ",";
        pts += string_format( "[%d,%d,%d,%s]", a.x(), a.y(), a.z(),
                              json_quote( string_from_color( col ) ) );
    }
    send_to_all_players( "{\"t\":\"anim\",\"k\":\"cexp\",\"n\":" + json_quote( name ) +
                         ",\"pts\":[" + pts + "]}" );
}

void share_revealed_overmap( const Character &who, const tripoint_abs_omt &center, int radius )
{
    if( !is_host() ) {
        return;
    }
    host::peer_t *p = host::peer_of_proxy( who.getID() );
    if( p == nullptr || !p->joined ) {
        return;
    }
    overmapbuffer &omb = get_overmapbuffer( who.get_dimension() );
    std::string tiles;
    for( int dx = -radius; dx <= radius; ++dx ) {
        for( int dy = -radius; dy <= radius; ++dy ) {
            const tripoint_abs_omt op( center.x() + dx, center.y() + dy, center.z() );
            if( !omb.seen( op ) ) {
                continue;
            }
            tiles += tiles.empty() ? "" : ",";
            tiles += string_format( "[%d,%d,%d,%s]", op.x(), op.y(), op.z(),
                                    json_quote( omb.ter( op ).id().str() ) );
        }
    }
    host::send( p->id, "{\"t\":\"reveal\",\"tiles\":[" + tiles + "]}" );
}

void share_sound_marker( const Character &who, const tripoint_bub_ms &p )
{
    if( !is_host() ) {
        return;
    }
    if( host::peer_t *peer = host::peer_of_proxy( who.getID() ) ) {
        peer->sound_marks.push_back( bub_to_abs( p ) );
    }
}

void host_tile_changed( const tripoint_abs_ms &p )
{
    if( !is_host() || host::peers.empty() ) {
        return;
    }
    const tripoint_abs_sm sm = project_to<coords::sm>( p );
    for( auto &[id, peer] : host::peers ) {
        if( peer.joined ) {
            peer.forced.insert( sm );
        }
    }
}

bool remote_prompts_active()
{
    return is_host() && host::prompt_peer >= 0;
}

void remote_message( const std::string &text )
{
    if( host::peer_t *p = host::find_peer( host::prompt_peer ) ) {
        host::send_to_player( *p, m_info, remove_color_tags( text ) );
    }
}

namespace
{
// Sends a question ("kind" plus its fields) to the player whose action is
// running and waits for the answer.  The rest of the game waits with us, as
// it would for a question on our own screen.  nullopt: the player left.
std::optional<std::string> ask_remote( const std::string &kind, const std::string &fields )
{
    const int peer_id = host::prompt_peer;
    host::peer_t *p = host::find_peer( peer_id );
    if( p == nullptr ) {
        return std::nullopt;
    }
    const std::string name = p->name;
    const int id = host::next_question_id++;
    host::send( peer_id, string_format( "{\"t\":\"ask\",\"id\":%d,\"kind\":%s%s}", id,
                                        json_quote( kind ), fields ) );
    static_popup notice;
    notice.on_top( true );
    while( true ) {
        // Questions asked while answering go to nobody else.
        host::prompt_scope none( -1 );
        pump();
        const auto it = host::answers.find( id );
        if( it != host::answers.end() ) {
            std::string answer = std::move( it->second );
            host::answers.erase( it );
            return answer;
        }
        p = host::find_peer( peer_id );
        if( p == nullptr || !p->joined ) {
            return std::nullopt;
        }
        notice.message( _( "Waiting for %s to answer a question…" ), name );
        ui_manager::redraw();
        refresh_display();
        inp_mngr.pump_events();
        std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
    }
}
} // namespace

int remote_choice( const std::string &text, const std::vector<std::string> &options,
                   const std::vector<bool> &enabled, bool allow_cancel )
{
    if( options.empty() ) {
        return -1;
    }
    std::string opts;
    std::string en;
    for( size_t i = 0; i < options.size(); ++i ) {
        opts += ( i ? "," : "" ) + json_quote( remove_color_tags( options[i] ) );
        en += ( i ? "," : "" ) + std::string( i < enabled.size() && !enabled[i] ? "false" : "true" );
    }
    const std::optional<std::string> answer = ask_remote( "choice",
            string_format( ",\"text\":%s,\"opts\":[%s],\"en\":[%s],\"cancel\":%s",
                           json_quote( remove_color_tags( text ) ), opts, en, allow_cancel ? "true" : "false" ) );
    if( !answer ) {
        return -1;
    }
    message m( *answer );
    const int pick = m.object().get_int( "i", -1 );
    return pick >= 0 && pick < static_cast<int>( options.size() ) ? pick : -1;
}

player *remote_actor()
{
    if( !remote_prompts_active() ) {
        return nullptr;
    }
    host::peer_t *p = host::find_peer( host::prompt_peer );
    return p != nullptr ? host::proxy_of( *p ) : nullptr;
}

std::optional<tripoint_bub_ms> remote_actor_pos()
{
    if( !remote_prompts_active() ) {
        return std::nullopt;
    }
    host::peer_t *p = host::find_peer( host::prompt_peer );
    npc *proxy = p != nullptr ? host::proxy_of( *p ) : nullptr;
    if( proxy == nullptr ) {
        return std::nullopt;
    }
    return proxy->bub_pos();
}

std::optional<tripoint_rel_ms> remote_direction( const std::string &prompt, bool allow_vertical )
{
    const std::optional<std::string> answer = ask_remote( "dir",
            string_format( ",\"text\":%s,\"vertical\":%s", json_quote( remove_color_tags( prompt ) ),
                           allow_vertical ? "true" : "false" ) );
    if( !answer ) {
        return std::nullopt;
    }
    message m( *answer );
    JsonObject jo = m.object();
    if( !jo.get_bool( "ok", false ) ) {
        return std::nullopt;
    }
    const tripoint_rel_ms dir( std::clamp( jo.get_int( "dx", 0 ), -1, 1 ),
                               std::clamp( jo.get_int( "dy", 0 ), -1, 1 ),
                               allow_vertical ? std::clamp( jo.get_int( "dz", 0 ), -1, 1 ) : 0 );
    // The character turns to what it deals with, as the avatar does.
    if( player *actor = remote_actor(); actor != nullptr && dir.x() != 0 ) {
        actor->facing = dir.x() > 0 ? FD_RIGHT : FD_LEFT;
    }
    return dir;
}

std::vector<std::pair<int, int>> remote_choose_many( const std::string &title,
                              const std::vector<std::string> &options, const std::vector<int> &max_counts )
{
    std::string opts;
    std::string maxes;
    for( size_t i = 0; i < options.size(); ++i ) {
        opts += ( i ? "," : "" ) + json_quote( remove_color_tags( options[i] ) );
        maxes += ( i ? "," : "" ) + std::to_string( i < max_counts.size() ? max_counts[i] : 1 );
    }
    std::vector<std::pair<int, int>> picks;
    const std::optional<std::string> answer = ask_remote( "multi",
            string_format( ",\"text\":%s,\"opts\":[%s],\"max\":[%s]",
                           json_quote( remove_color_tags( title ) ), opts, maxes ) );
    if( !answer ) {
        return picks;
    }
    message m( *answer );
    JsonObject jo = m.object();
    for( JsonArray pick : jo.get_array( "picks" ) ) {
        picks.emplace_back( pick.get_int( 0 ), pick.get_int( 1 ) );
    }
    return picks;
}

bool remote_vehicle_work( vehicle &veh, const tripoint_bub_ms &pos, Character &who )
{
    const tripoint_abs_ms abs = bub_to_abs( pos );
    const std::optional<std::string> answer = ask_remote( "vehexam",
            string_format( ",\"x\":%d,\"y\":%d,\"z\":%d", abs.x(), abs.y(), abs.z() ) );
    if( !answer ) {
        return false;
    }
    message m( *answer );
    JsonObject jo = m.object();
    if( !jo.get_bool( "ok", false ) ) {
        return false;
    }
    std::vector<int> values;
    jo.read( "values", values );
    if( values.size() < 7 ) {
        return false;
    }
    const int cmd = jo.get_int( "cmd", 0 );
    // The jobs the vehicle activity knows: install, repair, refill, remove.
    if( cmd != 'i' && cmd != 'r' && cmd != 'f' && cmd != 'o' ) {
        return false;
    }
    auto act = std::make_unique<player_activity>( activity_id( "ACT_VEHICLE" ),
               std::clamp( jo.get_int( "moves", 1000 ), 1, 10000000 ), cmd );
    for( const tripoint_abs_ms &p : veh.get_points( true ) ) {
        act->coord_set.insert( p );
    }
    act->values = values;
    act->str_values.push_back( jo.get_string( "part", "" ) );
    // The part (or other item) to install, from the player's things.
    if( jo.has_object( "item" ) ) {
        npc *guy = who.as_npc();
        JsonObject ij = jo.get_object( "item" );
        bool on_ground = false;
        item *it = guy != nullptr ? host::find_action_item( *guy, ij, on_ground ) : nullptr;
        if( it == nullptr ) {
            who.add_msg_if_player( m_info, _( "You no longer have that." ) );
            return false;
        }
        act->targets.emplace_back( it );
    }
    who.assign_activity( std::move( act ) );
    return true;
}

void remote_peek( const tripoint_rel_ms &dir )
{
    ask_remote( "peek", string_format( ",\"dx\":%d,\"dy\":%d,\"dz\":%d", dir.x(), dir.y(), dir.z() ) );
}

std::optional<tripoint_bub_ms> remote_target( const std::string &prompt, int range )
{
    const std::optional<std::string> answer = ask_remote( "target",
            string_format( ",\"text\":%s,\"range\":%d", json_quote( remove_color_tags( prompt ) ), range ) );
    if( !answer ) {
        return std::nullopt;
    }
    message m( *answer );
    JsonObject jo = m.object();
    if( !jo.get_bool( "ok", false ) ) {
        return std::nullopt;
    }
    const tripoint_bub_ms pos = host::read_bub( jo );
    if( !get_map().inbounds( pos ) ) {
        return std::nullopt;
    }
    return pos;
}

bool proxy_fire_fake_gun( Character &who, item &fake_gun, int64_t cost_per_shot_joules )
{
    player *actor = remote_actor();
    if( actor == nullptr || actor != &who ) {
        return false;
    }
    gun_mode mode = fake_gun.gun_current_mode();
    if( !mode ) {
        return true;
    }
    if( fake_gun.ammo_required() > 0 && fake_gun.ammo_remaining() == 0 ) {
        who.add_msg_if_player( m_info, _( "Your %s is empty." ), fake_gun.tname() );
        return true;
    }
    const units::energy cost = units::from_joule( cost_per_shot_joules );
    if( cost > 0_J && who.get_power_level() < cost ) {
        who.add_msg_if_player( m_info, _( "You don't have enough power to fire your %s." ),
                               fake_gun.tname() );
        return true;
    }
    const std::optional<tripoint_bub_ms> target = remote_target(
                string_format( _( "Fire your %s at:" ), fake_gun.tname() ),
                std::max( 1, fake_gun.gun_range( true ) ) );
    if( !target || *target == who.bub_pos() ) {
        return true;
    }
    const int shots = ranged::fire_gun( who, *target, mode.qty, *mode, nullptr );
    if( shots > 0 && cost > 0_J ) {
        who.mod_power_level( -cost * shots );
    }
    return true;
}

void remote_talk_begin()
{
    ++host::next_conversation_id;
}

std::optional<remote_talk_answer> remote_talk( const remote_talk_request &req )
{
    std::string resp;
    for( const talk_data &r : req.responses ) {
        resp += resp.empty() ? "" : ",";
        resp += string_format( "[%d,%s,%s]", static_cast<int>( r.letter ),
                               json_quote( string_from_color( r.col ) ), json_quote( r.text ) );
    }
    const std::optional<std::string> answer = ask_remote( "talk",
            string_format( ",\"conv\":%d,\"name\":%s,\"text\":%s,\"repeat\":%s,\"resp\":[%s]",
                           host::next_conversation_id, json_quote( req.npc_name ), json_quote( req.challenge ),
                           req.repeat ? "true" : "false", resp ) );
    if( !answer ) {
        return std::nullopt;
    }
    message m( *answer );
    JsonObject jo = m.object();
    const std::string special = jo.get_string( "sp", "" );
    if( !special.empty() ) {
        return remote_talk_answer{ .special = special.front() };
    }
    return remote_talk_answer{ .index = jo.get_int( "i", -1 ) };
}

namespace
{

// One side of a deal the remote player made: entries [index, type, name,
// amount].  The client built the same lists from its copies of both
// characters; an entry that does not match by index is looked up by type and
// name.  False if something they picked is not there any more.
bool apply_trade_picks( std::vector<item_pricing> &list, const JsonArray &picks, const bool theirs )
{
    for( JsonArray pick : picks ) {
        const int index = pick.get_int( 0 );
        const itype_id type( pick.get_string( 1 ) );
        const std::string name = pick.get_string( 2 );
        const int amount = pick.get_int( 3 );
        const auto matches = [&]( const item_pricing & ip ) {
            const item &it = *ip.locs.front();
            return !ip.selected && it.typeId() == type && it.display_name() == name;
        };
        item_pricing *ip = nullptr;
        if( index >= 0 && index < static_cast<int>( list.size() ) && matches( list[index] ) ) {
            ip = &list[index];
        } else {
            const auto found = std::ranges::find_if( list, matches );
            ip = found != list.end() ? &*found : nullptr;
        }
        if( ip == nullptr || amount <= 0 ) {
            return false;
        }
        const bool by_charges = ip->charges > 0;
        const int max_amount = by_charges ? ip->charges : std::max( ip->count, 1 );
        int &owner_sells = theirs ? ( by_charges ? ip->u_charges : ip->u_has ) :
                           ( by_charges ? ip->npc_charges : ip->npc_has );
        owner_sells = std::min( amount, max_amount );
        ip->selected = true;
    }
    return true;
}

} // namespace

bool remote_trade( npc &np, npc_trading::trade_state &state, const int cost,
                   const std::string &deal )
{
    player *actor = remote_actor();
    if( actor == nullptr ) {
        return false;
    }
    // Their copies of both characters must be current (a shopkeeper has just
    // restocked).
    host::broadcast( host::sync_level::light );
    const std::optional<std::string> answer = ask_remote( "trade",
            string_format( ",\"npc\":%d,\"cost\":%d,\"deal\":%s", np.getID().get_value(), cost,
                           json_quote( deal ) ) );
    if( !answer ) {
        return false;
    }
    message m( *answer );
    JsonObject jo = m.object();
    if( !jo.get_bool( "ok", false ) ) {
        return false;
    }
    if( !apply_trade_picks( state.theirs, jo.get_array( "theirs" ), true ) ||
        !apply_trade_picks( state.yours, jo.get_array( "yours" ), false ) ) {
        actor->add_msg_if_player( m_info,
                                  _( "The deal fell through: something changed hands meanwhile." ) );
        return false;
    }

    // The balance and the NPC's room, as the trade window keeps them.
    state.volume_left = np.is_shopkeeper() ? 5000_liter : np.volume_capacity() - np.volume_carried();
    state.weight_left = np.is_shopkeeper() ? 5000_kilogram : np.weight_capacity() - np.weight_carried();
    const auto amount_of = []( const item_pricing & ip, const bool theirs ) {
        if( ip.charges > 0 ) {
            return theirs ? ip.u_charges : ip.npc_charges;
        }
        return theirs ? ip.u_has : ip.npc_has;
    };
    const auto affects_npc_capacity = [&np]( const item & it ) {
        return it.where() == item_location_type::character && &it != &np.primary_weapon();
    };
    for( const bool theirs : { true, false } ) {
        for( const item_pricing &ip : theirs ? state.theirs : state.yours ) {
            if( !ip.selected ) {
                continue;
            }
            const int signed_amount = theirs ? amount_of( ip, true ) : -amount_of( ip, false );
            if( !np.will_exchange_items_freely() ) {
                state.your_balance -= static_cast<int>( ip.price * signed_amount );
            }
            if( affects_npc_capacity( *ip.locs.front() ) ) {
                state.volume_left += ip.vol * signed_amount;
                state.weight_left += ip.weight * signed_amount;
            }
        }
    }
    if( !npc_trading::npc_will_accept_trade( state, np ) ) {
        actor->add_msg_if_player( m_info, _( "%s doesn't accept that deal." ), np.get_name() );
        return false;
    }
    if( state.volume_left < 0_ml || state.weight_left < 0_gram ) {
        actor->add_msg_if_player( m_info, _( "%s can't carry all that." ), np.get_name() );
        return false;
    }
    return true;
}

void remote_show_overmap( const tripoint_abs_omt &center )
{
    player *actor = remote_actor();
    if( actor == nullptr ) {
        return;
    }
    // The overmap the player knows around that spot, and what is highlighted.
    constexpr int view_radius = 45;
    overmapbuffer &omb = get_overmapbuffer( actor->get_dimension() );
    std::string tiles;
    for( int dx = -view_radius; dx <= view_radius; ++dx ) {
        for( int dy = -view_radius; dy <= view_radius; ++dy ) {
            const tripoint_abs_omt op( center.x() + dx, center.y() + dy, center.z() );
            if( !omb.seen( op ) ) {
                continue;
            }
            tiles += tiles.empty() ? "" : ",";
            tiles += string_format( "[%d,%d,%d,%s]", op.x(), op.y(), op.z(),
                                    json_quote( omb.ter( op ).id().str() ) );
        }
    }
    std::string marks;
    for( const tripoint_abs_omt &op : uistate.overmap_highlighted_omts ) {
        marks += marks.empty() ? "" : ",";
        marks += string_format( "[%d,%d,%d]", op.x(), op.y(), op.z() );
    }
    ask_remote( "omap", string_format( ",\"x\":%d,\"y\":%d,\"z\":%d,\"tiles\":[%s],\"marks\":[%s]",
                                       center.x(), center.y(), center.z(), tiles, marks ) );
}

std::optional<tripoint_bub_ms> remote_tile( const std::string &prompt )
{
    const std::optional<std::string> answer = ask_remote( "tile",
            string_format( ",\"text\":%s", json_quote( remove_color_tags( prompt ) ) ) );
    if( !answer ) {
        return std::nullopt;
    }
    message m( *answer );
    JsonObject jo = m.object();
    if( !jo.get_bool( "ok", false ) ) {
        return std::nullopt;
    }
    const tripoint_bub_ms pos = host::read_bub( jo );
    if( !get_map().inbounds( pos ) ) {
        return std::nullopt;
    }
    return pos;
}

bool remote_pickup( const tripoint_bub_ms &pos )
{
    if( !remote_prompts_active() ) {
        return false;
    }
    host::peer_t *p = host::find_peer( host::prompt_peer );
    npc *proxy = p != nullptr ? host::proxy_of( *p ) : nullptr;
    if( proxy == nullptr ) {
        return true;
    }
    const tripoint_abs_ms abs = bub_to_abs( pos );
    const std::optional<std::string> answer = ask_remote( "pickup",
            string_format( ",\"x\":%d,\"y\":%d,\"z\":%d", abs.x(), abs.y(), abs.z() ) );
    if( !answer ) {
        return true;
    }
    message m( *answer );
    JsonObject jo = m.object();
    const host::action_result res = host::do_pickup( *proxy, pos, jo.get_array( "items" ) );
    if( !res.ok && !res.msg.empty() ) {
        proxy->add_msg_if_player( m_info, res.msg );
    }
    return true;
}

std::optional<std::string> remote_text( const std::string &title, const std::string &description,
                                        const std::string &initial, bool only_digits, int max_length )
{
    const std::optional<std::string> answer = ask_remote( "text",
            string_format( ",\"title\":%s,\"desc\":%s,\"init\":%s,\"digits\":%s,\"max\":%d",
                           json_quote( remove_color_tags( title ) ), json_quote( remove_color_tags( description ) ),
                           json_quote( initial ), only_digits ? "true" : "false", max_length ) );
    if( !answer ) {
        return std::nullopt;
    }
    message m( *answer );
    JsonObject jo = m.object();
    if( !jo.get_bool( "ok", false ) ) {
        return std::nullopt;
    }
    return jo.get_string( "s", "" );
}

bool host_blocks_action( action_id act )
{
    if( !is_host() ) {
        return false;
    }
    if( act == ACTION_QUICKLOAD ) {
        popup( _( "Quickload is disabled while hosting a co-op game." ) );
        return true;
    }
    return false;
}

void proxy_message( const npc &who, const game_message_params &params, const std::string &msg )
{
    if( !is_host() || msg.empty() ) {
        return;
    }
    if( host::peer_t *p = host::peer_of_proxy( who.getID() ) ) {
        host::send_to_player( *p, params.type, msg );
    }
}

void proxy_message( const npc &who, const std::string &msg )
{
    proxy_message( who, game_message_params( m_neutral ), msg );
}

} // namespace cata_mp
