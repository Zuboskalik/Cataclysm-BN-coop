#pragma once

// Co-op multiplayer transport layer.
//
// Newline-delimited JSON over TCP (standalone Asio).  This header deliberately
// pulls in no game headers: the implementation includes asio/winsock, which do
// not mix with the shared PCH, so everything game-related lives in mp_session.
//
// Threading: every socket operation runs on a private io thread.  The game
// thread talks to it through the thread-safe functions below and drains
// incoming traffic with poll().

#include <cstdint>
#include <string>
#include <vector>

namespace cata_mp::net
{

struct event {
    enum class kind : int {
        opened,  // host: a new peer connected
        line,    // a complete message arrived
        closed,  // the link to a peer (or, for a client, to the host) is gone
    };
    kind k = kind::line;
    // Host: session id of the peer.  Client: always 0.
    int peer = 0;
    // Message payload for kind::line, human readable reason for kind::closed,
    // remote address for kind::opened.
    std::string data;
};

// ---- host side ----------------------------------------------------------

// Opens a listening socket on the given TCP port (all IPv4 interfaces).
bool host_listen( uint16_t port, std::string &err );
// Closes the listener and every peer connection.
void host_shutdown();
bool host_listening();
uint16_t host_port();
// Queue one message for a peer.  The payload must not contain raw newlines
// (JSON output never does); large payloads are compressed automatically.
void host_send( int peer, const std::string &msg );
// Drop a peer connection (a closed event is still delivered).
void host_close_peer( int peer );
// Number of currently open peer sockets.
int host_peer_count();

// ---- client side --------------------------------------------------------

// Connect to a host with a bounded timeout.  On failure err holds the reason.
bool client_connect( const std::string &host, uint16_t port, int timeout_ms,
                     std::string &err );
void client_disconnect();
bool client_connected();
void client_send( const std::string &msg );
// Last measured heartbeat round-trip in milliseconds, -1 if unknown.
int client_rtt_ms();

// ---- common -------------------------------------------------------------

// Pop one pending network event.  Game thread only.
bool poll( event &out );
// Drop all pending events (used when a session ends).
void clear_events();

// Non-loopback IPv4 addresses of this machine, VPN-like ranges first.
std::vector<std::string> local_ipv4s();

int64_t now_ms();

} // namespace cata_mp::net
