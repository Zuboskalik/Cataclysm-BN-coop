#pragma once

// Internal declarations shared by mp_session.cpp, mp_host.cpp and mp_client.cpp.
// Not for use outside the co-op implementation; the game talks to mp_session.h.

#include <cstdint>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "json.h"

class Character;
class item;

namespace cata_mp
{

// Bumped whenever the wire format changes incompatibly.  Both ends must also
// run the very same build (see protocol_version_string()).
constexpr int protocol_version = 1;
constexpr uint16_t default_port = 8080;
// Remote players allowed at once (the host is the extra one).
constexpr int max_remote_players = 3;

enum class role : int {
    none,
    host,
    client,
};

struct session_settings {
    uint16_t port = default_port;
    std::string password;
    std::string last_address;
    std::vector<std::string> recent_addresses;
};

role current_role();
void set_role( role r );
session_settings &settings();
void save_settings();

// "<protocol>|<build version>" - must match exactly between host and client.
std::string protocol_version_string();

// ---- logging ----------------------------------------------------------------

// Appends a line to config/coop.log (truncated at session start).
void log( const std::string &line );
void log_reset();

// ---- JSON helpers -----------------------------------------------------------

std::string write_json( const std::function<void( JsonOut & )> &fn );
// Escapes a string for direct inclusion in hand-assembled JSON text.
std::string json_quote( const std::string &s );

// A parsed incoming message.  Keeps its source text alive for JsonIn.
class message
{
    public:
        explicit message( std::string text );
        bool valid() const {
            return valid_;
        }
        const std::string &type() const {
            return type_;
        }
        // Fresh JsonObject over the whole message (members not visited by the
        // caller are silently allowed).
        JsonObject object();
        const std::string &text() const {
            return text_;
        }
    private:
        std::string text_;
        // One stream per object() call: a JsonObject keeps pointing at the
        // JsonIn it came from until it is destroyed.
        std::vector<std::unique_ptr<std::istringstream>> streams_;
        std::vector<std::unique_ptr<JsonIn>> jsins_;
        std::string type_;
        bool valid_ = false;
};

// ---- item addressing --------------------------------------------------------

// Items carried by a character are addressed by their position in a pre-order
// walk of everything the character possesses.  The client's copy of its
// character is a deserialized snapshot of the proxy on the host, so both walks
// yield the same order; the item type id is sent along as a cross-check.
// The order can still drift (stacks merge differently once reloaded), so the
// client also sends which of the items of that type it means and its name, and
// the host falls back to those when the index does not match.
int item_index_of( Character &who, const item *it );
item *item_at_index( Character &who, int index );
// How many items of the same type come before `it` in the walk.
int item_type_ordinal( Character &who, const item *it );
item *find_carried_item( Character &who, int index, const std::string &type, int ordinal,
                         const std::string &name );
// Name used to tell apart items of one type.
std::string item_match_name( const item &it );

// ---- host side (mp_host.cpp) ------------------------------------------------

namespace host
{
bool listening();
bool start_listening( std::string &err );
void stop( const std::string &reason );
// One network message from a peer.
void handle_line( int peer, const std::string &line );
void handle_opened( int peer, const std::string &address );
void handle_closed( int peer, const std::string &reason );
void broadcast_chat( const std::string &from, const std::string &text );
int joined_count();
std::vector<std::string> joined_names();
void kick_menu();
std::string status_line();
} // namespace host

// ---- client side (mp_client.cpp) --------------------------------------------

namespace client
{
void handle_line( const std::string &line );
void handle_closed( const std::string &reason );
void send( const std::string &msg );
void send_chat( const std::string &text );
std::string status_line();
void leave_session( bool ask );
// Asks the host for submaps that had to be stubbed out locally.
void flush_placeholders();
void update_waiting_notice();
} // namespace client

} // namespace cata_mp
