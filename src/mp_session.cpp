#include "mp_session.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>

#include "avatar.h"
#include "character.h"
#include "debug.h"
#include "fstream_utils.h"
#include "game.h"
#include "get_version.h"
#include "input.h"
#include "item.h"
#include "json.h"
#include "messages.h"
#include "mp_common.h"
#include "mp_net.h"
#include "npc.h"
#include "output.h"
#include "path_info.h"
#include "popup.h"
#include "string_formatter.h"
#include "string_input_popup.h"
#include "translations.h"
#include "ui.h"
#include "ui_manager.h"

namespace cata_mp
{

const std::string client_world_name = "Co-op (client)";

// ============================================================================
// Session state
// ============================================================================

namespace
{
// Read from map loading worker threads too (suppress_world_simulation).
std::atomic<role> g_role{ role::none };
bool g_host_armed = false;
session_settings g_settings;
bool g_settings_loaded = false;

std::string settings_path()
{
    return PATH_INFO::config_dir() + "coop.json";
}

std::string log_path()
{
    return PATH_INFO::config_dir() + "coop.log";
}

void load_settings()
{
    if( g_settings_loaded ) {
        return;
    }
    g_settings_loaded = true;
    read_from_file_json( settings_path(), []( JsonIn & jsin ) {
        JsonObject jo = jsin.get_object();
        jo.allow_omitted_members();
        int port = default_port;
        if( jo.read( "port", port ) && port > 0 && port < 65536 ) {
            g_settings.port = static_cast<uint16_t>( port );
        }
        jo.read( "password", g_settings.password );
        jo.read( "last_address", g_settings.last_address );
        jo.read( "recent_addresses", g_settings.recent_addresses );
    }, true );
}
} // namespace

role current_role()
{
    return g_role;
}

void set_role( role r )
{
    g_role = r;
}

session_settings &settings()
{
    load_settings();
    return g_settings;
}

void save_settings()
{
    load_settings();
    write_to_file( settings_path(), []( std::ostream & fout ) {
        JsonOut jsout( fout, true );
        jsout.start_object();
        jsout.member( "port", static_cast<int>( g_settings.port ) );
        jsout.member( "password", g_settings.password );
        jsout.member( "last_address", g_settings.last_address );
        jsout.member( "recent_addresses", g_settings.recent_addresses );
        jsout.end_object();
    }, _( "co-op settings" ) );
}

std::string protocol_version_string()
{
    return string_format( "%d|%s", protocol_version, getVersionString() );
}

// ============================================================================
// Logging
// ============================================================================

void log_reset()
{
    std::ofstream out( log_path(), std::ios::trunc );
}

void log( const std::string &line )
{
    std::ofstream out( log_path(), std::ios::app );
    if( !out ) {
        return;
    }
    const std::time_t now = std::time( nullptr );
    char stamp[32] = {};
    std::strftime( stamp, sizeof( stamp ), "%H:%M:%S", std::localtime( &now ) );
    out << stamp << " " << line << "\n";
}

// ============================================================================
// JSON helpers
// ============================================================================

std::string write_json( const std::function<void( JsonOut & )> &fn )
{
    std::ostringstream os;
    {
        JsonOut jsout( os );
        fn( jsout );
    }
    return os.str();
}

std::string json_quote( const std::string &s )
{
    return write_json( [&s]( JsonOut & jsout ) {
        jsout.write( s );
    } );
}

message::message( std::string text ) : text_( std::move( text ) )
{
    try {
        JsonObject jo = object();
        jo.allow_omitted_members();
        if( jo.has_string( "t" ) ) {
            type_ = jo.get_string( "t" );
            valid_ = true;
        }
    } catch( const std::exception &e ) {
        log( std::string( "bad message: " ) + e.what() + " :: " + text_.substr( 0, 200 ) );
        valid_ = false;
    }
}

JsonObject message::object()
{
    streams_.push_back( std::make_unique<std::istringstream>( text_ ) );
    jsins_.push_back( std::make_unique<JsonIn>( *streams_.back() ) );
    JsonObject jo = jsins_.back()->get_object();
    jo.allow_omitted_members();
    return jo;
}

// ============================================================================
// Item addressing
// ============================================================================

int item_index_of( Character &who, const item *it )
{
    if( it == nullptr ) {
        return -1;
    }
    int index = 0;
    int found = -1;
    who.visit_items( [&]( const item * node ) {
        if( node == it ) {
            found = index;
            return VisitResponse::ABORT;
        }
        ++index;
        return VisitResponse::NEXT;
    } );
    return found;
}

item *item_at_index( Character &who, const int index )
{
    if( index < 0 ) {
        return nullptr;
    }
    int cur = 0;
    item *found = nullptr;
    who.visit_items( [&]( item * node ) {
        if( cur == index ) {
            found = node;
            return VisitResponse::ABORT;
        }
        ++cur;
        return VisitResponse::NEXT;
    } );
    return found;
}

std::string item_match_name( const item &it )
{
    return remove_color_tags( it.tname( 1, false ) );
}

int item_type_ordinal( Character &who, const item *it )
{
    if( it == nullptr ) {
        return -1;
    }
    int ordinal = 0;
    int found = -1;
    who.visit_items( [&]( const item * node ) {
        if( node == it ) {
            found = ordinal;
            return VisitResponse::ABORT;
        }
        if( node->typeId() == it->typeId() ) {
            ++ordinal;
        }
        return VisitResponse::NEXT;
    } );
    return found;
}

item *find_carried_item( Character &who, const int index, const std::string &type,
                         const int ordinal, const std::string &name )
{
    item *const exact = item_at_index( who, index );
    if( exact != nullptr && exact->typeId().str() == type &&
        ( name.empty() || item_match_name( *exact ) == name ) ) {
        return exact;
    }
    std::vector<item *> same_type;
    who.visit_items( [&]( item * node ) {
        if( node->typeId().str() == type ) {
            same_type.push_back( node );
        }
        return VisitResponse::NEXT;
    } );
    if( same_type.empty() ) {
        return nullptr;
    }
    std::vector<item *> same_name;
    for( item *candidate : same_type ) {
        if( !name.empty() && item_match_name( *candidate ) == name ) {
            same_name.push_back( candidate );
        }
    }
    const std::vector<item *> &pool = same_name.empty() ? same_type : same_name;
    if( ordinal >= 0 && static_cast<size_t>( ordinal ) < same_type.size() &&
        std::find( pool.begin(), pool.end(), same_type[ordinal] ) != pool.end() ) {
        return same_type[ordinal];
    }
    if( exact != nullptr && std::find( pool.begin(), pool.end(), exact ) != pool.end() ) {
        return exact;
    }
    return pool.front();
}

// ============================================================================
// Public queries
// ============================================================================

bool active()
{
    return g_role != role::none;
}

bool is_host()
{
    return g_role == role::host;
}

bool is_client()
{
    return g_role == role::client;
}

bool is_proxy( const Character &who )
{
    return who.is_npc() && !who.get_value( "coop_player" ).empty();
}

int connected_players()
{
    return is_host() ? host::joined_count() : 0;
}

bool suppress_world_simulation()
{
    return g_role == role::client;
}

bool suppress_time_skipping()
{
    return g_role == role::host && host::joined_count() > 0;
}

// ============================================================================
// Main menu
// ============================================================================

void arm_host()
{
    g_host_armed = true;
}

void disarm_host()
{
    g_host_armed = false;
}

bool host_armed()
{
    return g_host_armed;
}

bool configure_host()
{
    session_settings &s = settings();
    string_input_popup port_popup;
    port_popup.title( _( "TCP port to listen on:" ) )
    .description( _( "Your partner connects to this port.  Allow it through the firewall when Windows asks." ) )
    .width( 8 )
    .only_digits( true )
    .text( std::to_string( s.port ) );
    port_popup.query();
    if( port_popup.canceled() ) {
        return false;
    }
    const int port = std::atoi( port_popup.text().c_str() );
    if( port <= 0 || port >= 65536 ) {
        popup( _( "Invalid port number." ) );
        return false;
    }
    string_input_popup pw_popup;
    pw_popup.title( _( "Session password (empty for none):" ) )
    .width( 30 )
    .text( s.password );
    pw_popup.query();
    if( pw_popup.canceled() ) {
        return false;
    }
    s.port = static_cast<uint16_t>( port );
    s.password = pw_popup.text();
    save_settings();
    return true;
}

std::string menu_hint()
{
    const session_settings &s = settings();
    return string_format( _( "Host: TCP port %d.  Join: by IP address, e.g. 192.168.1.20:%d." ),
                          s.port, s.port );
}

// ============================================================================
// Network pump
// ============================================================================

bool pump()
{
    if( g_role == role::none ) {
        return false;
    }
    bool changed = false;
    net::event ev;
    // Bounded so a flood of traffic cannot starve the UI.
    for( int budget = 0; budget < 256 && net::poll( ev ); ++budget ) {
        changed = true;
        if( g_role == role::host ) {
            switch( ev.k ) {
                case net::event::kind::opened:
                    host::handle_opened( ev.peer, ev.data );
                    break;
                case net::event::kind::line:
                    host::handle_line( ev.peer, ev.data );
                    break;
                case net::event::kind::closed:
                    host::handle_closed( ev.peer, ev.data );
                    break;
            }
        } else if( g_role == role::client ) {
            switch( ev.k ) {
                case net::event::kind::opened:
                    break;
                case net::event::kind::line:
                    client::handle_line( ev.data );
                    break;
                case net::event::kind::closed:
                    client::handle_closed( ev.data );
                    break;
            }
        } else {
            break;
        }
    }
    if( g_role == role::client ) {
        client::flush_placeholders();
        client::update_waiting_notice();
    }
    return changed;
}

void on_turn_start()
{
    if( g_host_armed && g_role == role::none ) {
        g_host_armed = false;
        log_reset();
        std::string err;
        if( host::start_listening( err ) ) {
            set_role( role::host );
            const std::vector<std::string> ips = net::local_ipv4s();
            std::string addr_list;
            for( const std::string &ip : ips ) {
                addr_list += addr_list.empty() ? "" : ", ";
                addr_list += string_format( "%s:%d", ip, settings().port );
            }
            add_msg( m_good, _( "Co-op: hosting on TCP port %d.  Your partner can join at: %s" ),
                     settings().port, addr_list.empty() ? _( "(no network address found)" ) : addr_list );
            add_msg( m_info, _( "Co-op: %s for the co-op menu, %s to chat." ),
                     press_x( ACTION_COOP_MENU ), press_x( ACTION_COOP_CHAT ) );
        } else {
            popup( _( "Could not start hosting on port %d:\n%s\n\nThe game continues in single player." ),
                   settings().port, err );
        }
    }
    if( g_role == role::host ) {
        pump();
    }
}

// ============================================================================
// UI
// ============================================================================

void open_chat()
{
    if( g_role == role::none ) {
        add_msg( m_info, _( "Co-op chat is only available in a co-op session." ) );
        return;
    }
    string_input_popup chat_popup;
    chat_popup.title( _( "Say to your co-op partners:" ) )
    .width( 60 )
    .max_length( 200 )
    .identifier( "coop_chat" );
    chat_popup.query();
    const std::string text = chat_popup.text();
    if( chat_popup.canceled() || text.empty() ) {
        return;
    }
    if( g_role == role::host ) {
        host::broadcast_chat( get_avatar().get_name(), text );
    } else {
        client::send_chat( text );
    }
}

void open_menu()
{
    if( g_role == role::none ) {
        uilist menu;
        menu.text = _( "No co-op session is running.  Start one from the main menu (Co-op tab)." );
        menu.addentry( 0, true, 'q', _( "Close" ) );
        menu.query();
        return;
    }
    uilist menu;
    menu.title = _( "Co-op" );
    menu.text = status_line();
    if( g_role == role::host ) {
        std::string addresses;
        for( const std::string &ip : net::local_ipv4s() ) {
            addresses += string_format( "\n  %s:%d", ip, net::host_port() );
        }
        menu.text += string_format( _( "\n\nThis computer can be reached at:%s" ), addresses );
        menu.text += _( "\n(Over the internet use port forwarding, Radmin VPN, Tailscale, ZeroTier or playit.gg.)" );
        menu.addentry( 0, true, 'c', _( "Chat" ) );
        menu.addentry( 1, host::joined_count() > 0, 'k', _( "Disconnect a player" ) );
        menu.addentry( 9, true, 'q', _( "Close" ) );
    } else {
        menu.addentry( 0, true, 'c', _( "Chat" ) );
        menu.addentry( 2, true, 'l', _( "Leave the session" ) );
        menu.addentry( 9, true, 'q', _( "Close" ) );
    }
    menu.query();
    switch( menu.ret ) {
        case 0:
            open_chat();
            break;
        case 1:
            host::kick_menu();
            break;
        case 2:
            client::leave_session( true );
            break;
        default:
            break;
    }
}

std::string status_line()
{
    switch( g_role ) {
        case role::host:
            return host::status_line();
        case role::client:
            return client::status_line();
        case role::none:
            break;
    }
    return std::string();
}

// ============================================================================
// Teardown
// ============================================================================

void on_game_end()
{
    g_host_armed = false;
    if( g_role == role::host ) {
        host::stop( _( "The host has ended the session." ) );
    } else if( g_role == role::client ) {
        client::leave_session( false );
    }
    set_role( role::none );
    net::clear_events();
}

} // namespace cata_mp
