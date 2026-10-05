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

#include "avatar.h"
#include "calendar.h"
#include "character.h"
#include "character_turn.h"
#include "creature.h"
#include "debug.h"
#include "effect.h"
#include "faction.h"
#include "game.h"
#include "game_constants.h"
#include "construction.h"
#include "gates.h"
#include "iexamine.h"
#include "gun_mode.h"
#include "input.h"
#include "item.h"
#include "line.h"
#include "map/map.h"
#include "map/mapbuffer.h"
#include "map/mapdata.h"
#include "map/submap.h"
#include "messages.h"
#include "map/field_type.h"
#include "monster.h"
#include "mtype.h"
#include "mp_common.h"
#include "mp_net.h"
#include "npc.h"
#include "overmap/omdata.h"
#include "output.h"
#include "overmap/overmapbuffer.h"
#include "player_activity.h"
#include "popup.h"
#include "ranged.h"
#include "recipe.h"
#include "requirements.h"
#include "itype.h"
#include "reload/reload_selection.h"
#include "string_formatter.h"
#include "string_utils.h"
#include "translations.h"
#include "trap.h"
#include "type_id.h"
#include "ui.h"
#include "ui_manager.h"
#include "vehicle/vehicle.h"
#include "weather/weather.h"
#include "vehicle/vpart_position.h"
#include "worldfactory.h"
#include "world.h"

static const efftype_id effect_npc_suspend( "npc_suspend" );
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
    // The host chose not to wait for this player: their character idles
    // until they send an action.
    bool dont_wait = false;

    // What this client already has, by content hash.
    std::map<tripoint_abs_sm, size_t> sm_sent;
    std::set<int> synced_z;
    size_t rolling_cursor = 0;
    std::map<std::string, size_t> mon_sent;
    std::map<int, size_t> npc_sent;
    size_t you_sent = 0;
    std::map<tripoint_abs_omt, std::string> omt_sent;
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
std::map<int, int> answers;

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

struct broadcast_cache {
    std::map<tripoint_abs_sm, std::pair<size_t, std::string>> submaps;
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

    // ---- own character
    {
        const std::string you = write_json( [proxy]( JsonOut & jsout ) {
            proxy->serialize( jsout );
        } );
        const size_t h = std::hash<std::string>()( you );
        if( h != p.you_sent || p.full_sync ) {
            p.you_sent = h;
            out += ",\"you\":" + you;
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
            std::string json = write_json( [&critter]( JsonOut & jsout ) {
                critter.serialize( jsout );
            } );
            const size_t h = std::hash<std::string>()( json );
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
            std::string json = write_json( writer );
            const size_t h = std::hash<std::string>()( json );
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
    if( joined_count() >= max_remote_players ) {
        send_error( p.id, string_format( _( "The session is full (%d remote players)." ),
                                         max_remote_players ) );
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
        // An offline player's character stays where it is, out of time.
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

action_result do_move( npc &guy, int dx, int dy )
{
    map &here = get_map();
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
    guy.move_to( dest, true );
    if( guy.get_moves() == moves_before && guy.bub_pos() != dest ) {
        return fail( _( "You can't move there." ) );
    }
    return action_result{};
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
std::vector<std::pair<item *, int>> resolve_ground_items( const tripoint_bub_ms &pos,
        const JsonArray &list )
{
    const std::vector<item *> ground = ground_items( pos );
    std::vector<std::pair<item *, int>> result;
    for( JsonArray entry : list ) {
        const int count = entry.get_int( 1 );
        item *it = resolve_ground_item( ground, entry.get_int( 0 ), entry.get_string( 2 ),
                                        entry.size() > 3 ? entry.get_int( 3 ) : -1 );
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
        detached_ptr<item> taken = ( qty > 0 && qty < it->charges ) ? it->split( qty ) :
                                   here.i_rem( pos, it );
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

bool hostile_in_view( npc &guy, std::string &what )
{
    for( monster &critter : g->all_monsters() ) {
        if( critter.is_dead() || rl_dist( critter.bub_pos(), guy.bub_pos() ) > 20 ) {
            continue;
        }
        if( critter.attitude_to( guy ) == Attitude::A_HOSTILE && guy.sees( critter ) ) {
            what = critter.get_name();
            return true;
        }
    }
    return false;
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
    if( here.veh_at( pos ) ) {
        return fail( _( "Using vehicles is not available in co-op yet." ) );
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

// "Read" (R).  NPCs read only to learn a skill, so books just for fun
// can't be read this way yet.
action_result do_read( npc &guy, const JsonObject &jo )
{
    item *book = find_carried_item( guy, jo.get_int( "idx", -1 ), jo.get_string( "type", "" ),
                                    jo.get_int( "ord", -1 ), jo.get_string( "name", "" ) );
    if( book == nullptr ) {
        return fail( _( "You no longer have that." ) );
    }
    std::vector<std::string> reasons;
    if( !guy.can_read( *book, reasons ) ) {
        std::string why;
        for( const std::string &r : reasons ) {
            why += why.empty() ? r : "  " + r;
        }
        return fail( why.empty() ? _( "You can't read that." ) : why );
    }
    guy.start_read( *book, &guy );
    guy.add_msg_if_player( m_info, _( "Now reading %s." ), book->type_name() );
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
    item *it = find_carried_item( guy, jo.get_int( "idx", -1 ), jo.get_string( "type", "" ),
                                  jo.get_int( "ord", -1 ), jo.get_string( "name", "" ) );
    if( it == nullptr ) {
        return fail( _( "You no longer have that." ) );
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

action_result execute( peer_t &p, npc &guy, message &m )
{
    JsonObject jo = m.object();
    const std::string a = jo.get_string( "a", "" );
    if( a == "move" ) {
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
        const int mode = jo.get_int( "mode", CMM_WALK );
        if( mode < CMM_WALK || mode >= CMM_COUNT ) {
            return fail( std::string() );
        }
        guy.set_movement_mode( static_cast<character_movemode>( mode ) );
        return action_result{};
    }
    return fail( _( "That action is not available in co-op yet." ) );
}

// Waits for the next action of player `peer_id`.  Returns false if the player
// left, or the host decided to skip the turn.
bool wait_for_action( int peer_id )
{
    const int64_t started = net::now_ms();
    int64_t last_turn_sent = started;
    std::unique_ptr<static_popup> notice;
    input_context ctxt( "COOP_WAIT" );
    ctxt.register_action( "COOP_WAIT_MENU" );
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
                             p->name, secs, ctxt.get_desc( "COOP_WAIT_MENU" ) );
        }
        ui_manager::redraw();
        refresh_display();
        ctxt.set_timeout( 40 );
        const std::string action = ctxt.handle_input();
        if( action == "COOP_WAIT_MENU" ) {
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
        answers[jo.get_int( "id", 0 )] = jo.get_int( "i", -1 );
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
    return string_format( _( "Co-op: hosting on port %d.  Players: %s" ), net::host_port(), list );
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
    if( !is_host() ) {
        return;
    }
    pump();
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
    int guard = 0;
    while( !guy.is_dead() && guy.get_moves() > 0 && guard++ < 64 ) {
        p = host::find_peer( peer_id );
        if( p == nullptr || !p->joined ) {
            guy.set_moves( 0 );
            break;
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
                guy.set_moves( 0 );
            }
            continue;
        }
        if( p->auto_wait_turns > 0 ) {
            std::string what;
            if( host::hostile_in_view( guy, what ) ) {
                p->auto_wait_turns = 0;
                guy.add_msg_if_player( m_warning, _( "You stop waiting: you see a %s!" ), what );
                continue;
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
        message m( *p->action );
        p->action.reset();
        p->turn_announced = false;
        int seq = 0;
        host::action_result result;
        try {
            JsonObject jo = m.object();
            seq = jo.get_int( "seq", 0 );
            host::prompt_scope questions( peer_id );
            result = host::execute( *p, guy, m );
        } catch( const std::exception &e ) {
            log( std::string( "action failed: " ) + e.what() );
            result = host::fail( _( "The host could not process that action." ) );
        }
        if( !result.ok && !result.msg.empty() ) {
            host::send_to_player( *p, m_info, result.msg );
        }
        host::broadcast( host::sync_level::normal );
        host::send( peer_id, string_format( "{\"t\":\"ack\",\"seq\":%d,\"ok\":%s}", seq,
                                            result.ok ? "true" : "false" ) );
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

int remote_choice( const std::string &text, const std::vector<std::string> &options,
                   const std::vector<bool> &enabled, bool allow_cancel )
{
    const int peer_id = host::prompt_peer;
    host::peer_t *p = host::find_peer( peer_id );
    if( p == nullptr || options.empty() ) {
        return -1;
    }
    const std::string name = p->name;
    const int id = host::next_question_id++;
    std::string opts;
    std::string en;
    for( size_t i = 0; i < options.size(); ++i ) {
        opts += ( i ? "," : "" ) + json_quote( remove_color_tags( options[i] ) );
        en += ( i ? "," : "" ) + std::string( i < enabled.size() && !enabled[i] ? "false" : "true" );
    }
    host::send( peer_id, string_format( "{\"t\":\"ask\",\"id\":%d,\"text\":%s,\"opts\":[%s],\"en\":[%s],"
                                        "\"cancel\":%s}", id, json_quote( remove_color_tags( text ) ), opts, en,
                                        allow_cancel ? "true" : "false" ) );
    // Wait for the answer; the rest of the game waits with us, as it would
    // for a question on our own screen.
    static_popup notice;
    notice.on_top( true );
    while( true ) {
        // Questions asked while answering go to nobody else.
        host::prompt_scope none( -1 );
        pump();
        const auto it = host::answers.find( id );
        if( it != host::answers.end() ) {
            const int answer = it->second;
            host::answers.erase( it );
            return answer >= 0 && answer < static_cast<int>( options.size() ) ? answer : -1;
        }
        p = host::find_peer( peer_id );
        if( p == nullptr || !p->joined ) {
            return -1;
        }
        notice.message( _( "Waiting for %s to answer a question…" ), name );
        ui_manager::redraw();
        refresh_display();
        inp_mngr.pump_events();
        std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
    }
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
