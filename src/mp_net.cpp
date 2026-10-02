// Co-op multiplayer transport.  See mp_net.h.
//
// This translation unit is compiled without the shared PCH (see
// src/CMakeLists.txt) because standalone Asio drags in winsock2.h/windows.h.
// Keep game headers out of here.

#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0A00
#endif
#define ASIO_STANDALONE
#define ASIO_NO_DEPRECATED

#include "mp_net.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <istream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include <asio.hpp>
#include <zlib.h>

#if defined(_WIN32)
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

#include "catacharset.h"

using asio::ip::tcp;

namespace cata_mp::net
{

namespace
{

constexpr int64_t heartbeat_interval_ms = 1500;
// A peer that stays completely silent this long is considered gone.  The far
// end heartbeats every 1.5s from its io thread, so this only trips on a
// genuinely dead link, never on a player who is just thinking.
constexpr int64_t stall_timeout_ms = 20000;
// Messages shorter than this are sent as plain text.
constexpr size_t compress_threshold = 1024;

const std::string heartbeat_prefix = "{\"t\":\"hb\"";
const std::string compressed_prefix = "{\"z\":\"";

// Optional transport trace (set CATA_COOP_NETLOG to a file path).
void netlog( const std::string &what )
{
    static const char *path = std::getenv( "CATA_COOP_NETLOG" );
    if( path == nullptr ) {
        return;
    }
    static std::mutex m;
    std::lock_guard<std::mutex> lk( m );
    if( FILE *f = std::fopen( path, "a" ) ) {
        std::fprintf( f, "%lld %s\n", static_cast<long long>( std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch() ).count() ), what.c_str() );
        std::fclose( f );
    }
}

// ---- event queue (io thread -> game thread) -----------------------------

std::mutex events_mutex;
std::deque<event> events;

void push_event( event::kind k, int peer, std::string data )
{
    std::lock_guard<std::mutex> lk( events_mutex );
    events.push_back( event{ k, peer, std::move( data ) } );
}

// ---- framing ------------------------------------------------------------

std::string compress_frame( const std::string &msg )
{
    if( msg.size() < compress_threshold ) {
        return msg + "\n";
    }
    uLongf bound = compressBound( static_cast<uLong>( msg.size() ) );
    std::string buf;
    buf.resize( 4 + bound );
    const uint32_t raw_size = static_cast<uint32_t>( msg.size() );
    for( int i = 0; i < 4; ++i ) {
        buf[i] = static_cast<char>( ( raw_size >> ( 8 * i ) ) & 0xFF );
    }
    const int rc = compress2( reinterpret_cast<Bytef *>( &buf[4] ), &bound,
                              reinterpret_cast<const Bytef *>( msg.data() ),
                              static_cast<uLong>( msg.size() ), Z_BEST_SPEED );
    if( rc != Z_OK || bound + 4 >= msg.size() ) {
        return msg + "\n";
    }
    buf.resize( 4 + bound );
    return compressed_prefix + base64_encode( buf ) + "\"}\n";
}

// Returns false when the frame is corrupt.
bool decompress_frame( std::string &line )
{
    if( line.size() < compressed_prefix.size() + 2 ||
        line.compare( 0, compressed_prefix.size(), compressed_prefix ) != 0 ) {
        return true;
    }
    const size_t end = line.rfind( '"' );
    if( end == std::string::npos || end <= compressed_prefix.size() ) {
        return false;
    }
    const std::string packed = base64_decode( line.substr( compressed_prefix.size(),
                               end - compressed_prefix.size() ) );
    if( packed.size() < 4 ) {
        return false;
    }
    uint32_t raw_size = 0;
    for( int i = 0; i < 4; ++i ) {
        raw_size |= static_cast<uint32_t>( static_cast<unsigned char>( packed[i] ) ) << ( 8 * i );
    }
    // Sanity limit: no legitimate message is anywhere near this big.
    if( raw_size > 256u * 1024u * 1024u ) {
        return false;
    }
    std::string out;
    out.resize( raw_size );
    uLongf out_len = raw_size;
    const int rc = uncompress( reinterpret_cast<Bytef *>( out.data() ), &out_len,
                               reinterpret_cast<const Bytef *>( packed.data() + 4 ),
                               static_cast<uLong>( packed.size() - 4 ) );
    if( rc != Z_OK || out_len != raw_size ) {
        return false;
    }
    line = std::move( out );
    return true;
}

const std::string pong_prefix = "{\"t\":\"pong\"";

bool is_heartbeat( const std::string &line )
{
    return line.compare( 0, heartbeat_prefix.size(), heartbeat_prefix ) == 0 ||
           line.compare( 0, pong_prefix.size(), pong_prefix ) == 0;
}

long long read_number_after( const std::string &line, const char *key )
{
    const size_t p = line.find( key );
    if( p == std::string::npos ) {
        return -1;
    }
    return std::strtoll( line.c_str() + p + std::strlen( key ), nullptr, 10 );
}

// System error texts are localized in the ANSI code page on Windows, which the
// UTF-8 UI cannot show; describe the common failures ourselves.
std::string describe( const asio::error_code &ec )
{
    if( ec == asio::error::connection_refused ) {
        return "connection refused (is the host in the game and the port open?)";
    }
    if( ec == asio::error::timed_out ) {
        return "connection timed out";
    }
    if( ec == asio::error::host_unreachable || ec == asio::error::network_unreachable ) {
        return "host unreachable";
    }
    if( ec == asio::error::host_not_found || ec == asio::error::host_not_found_try_again ) {
        return "unknown host name";
    }
    if( ec == asio::error::eof || ec == asio::error::connection_reset ||
        ec == asio::error::connection_aborted ) {
        return "connection closed by the other side";
    }
    if( ec == asio::error::address_in_use ) {
        return "the port is already in use";
    }
    if( ec == asio::error::access_denied ) {
        return "access denied (port blocked?)";
    }
    return "network error " + std::to_string( ec.value() );
}

// ---- a single TCP connection --------------------------------------------

struct connection : std::enable_shared_from_this<connection> {
    tcp::socket socket;
    asio::streambuf read_buf;
    std::deque<std::string> write_queue;
    bool writing = false;
    bool closed = false;
    int id = 0;
    std::atomic<int64_t> last_recv_ms{ 0 };
    // Called on the io thread for every non-heartbeat line, and once on close.
    std::function<void( const std::shared_ptr<connection> &, std::string & )> on_line;
    std::function<void( const std::shared_ptr<connection> &, const std::string & )> on_close;
    std::function<void( const std::shared_ptr<connection> &, const std::string & )> on_heartbeat;

    explicit connection( tcp::socket s ) : socket( std::move( s ) ) {}

    void start() {
        asio::error_code ec;
        // Lockstep traffic is many tiny packets: never let Nagle batch them.
        socket.set_option( tcp::no_delay( true ), ec );
        socket.set_option( asio::socket_base::keep_alive( true ), ec );
        last_recv_ms = now_ms();
        do_read();
    }

    void send_raw( std::string framed ) {
        if( closed ) {
            return;
        }
        write_queue.push_back( std::move( framed ) );
        if( !writing ) {
            do_write();
        }
    }

    void do_write() {
        if( write_queue.empty() || closed ) {
            writing = false;
            return;
        }
        writing = true;
        // Coalesce everything queued into one write.
        auto buf = std::make_shared<std::string>();
        for( const std::string &m : write_queue ) {
            buf->append( m );
        }
        write_queue.clear();
        auto self = shared_from_this();
        asio::async_write( socket, asio::buffer( *buf ),
        [self, buf]( const asio::error_code & ec, std::size_t ) {
            if( ec ) {
                self->close( "write failed: " + describe( ec ) );
                return;
            }
            self->do_write();
        } );
    }

    void do_read() {
        auto self = shared_from_this();
        asio::async_read_until( socket, read_buf, '\n',
        [self]( const asio::error_code & ec, std::size_t ) {
            if( ec ) {
                self->close( describe( ec ) );
                return;
            }
            self->last_recv_ms = now_ms();
            netlog( "read conn=" + std::to_string( self->id ) + " bytes=" + std::to_string( self->read_buf.size() ) );
            std::istream is( &self->read_buf );
            std::string line;
            std::getline( is, line );
            if( !line.empty() && line.back() == '\r' ) {
                line.pop_back();
            }
            if( !line.empty() ) {
                if( is_heartbeat( line ) ) {
                    if( self->on_heartbeat ) {
                        self->on_heartbeat( self, line );
                    }
                } else if( decompress_frame( line ) ) {
                    if( self->on_line ) {
                        self->on_line( self, line );
                    }
                }
            }
            if( !self->closed ) {
                self->do_read();
            }
        } );
    }

    void close( const std::string &why ) {
        if( closed ) {
            return;
        }
        closed = true;
        asio::error_code ec;
        socket.shutdown( tcp::socket::shutdown_both, ec );
        socket.close( ec );
        if( on_close ) {
            on_close( shared_from_this(), why );
        }
    }
};

// ---- host ---------------------------------------------------------------

struct host_impl {
    asio::io_context io;
    tcp::acceptor acceptor{ io };
    asio::steady_timer hb_timer{ io };
    std::thread thread;
    std::map<int, std::shared_ptr<connection>> peers;
    std::atomic<int> peer_count{ 0 };
    int next_id = 1;
    uint16_t port = 0;

    ~host_impl() {
        io.stop();
        if( thread.joinable() ) {
            thread.join();
        }
    }

    void do_accept() {
        acceptor.async_accept( [this]( const asio::error_code & ec, tcp::socket sock ) {
            if( ec ) {
                if( ec != asio::error::operation_aborted ) {
                    do_accept();
                }
                return;
            }
            auto c = std::make_shared<connection>( std::move( sock ) );
            c->id = next_id++;
            std::string addr = "unknown";
            asio::error_code rec;
            const tcp::endpoint ep = c->socket.remote_endpoint( rec );
            if( !rec ) {
                addr = ep.address().to_string() + ":" + std::to_string( ep.port() );
            }
            c->on_line = []( const std::shared_ptr<connection> &self, std::string & line ) {
                push_event( event::kind::line, self->id, std::move( line ) );
            };
            c->on_heartbeat = []( const std::shared_ptr<connection> &self, const std::string & line ) {
                // Echo the client's stamp straight back from the io thread so the
                // measured round trip is pure network latency.
                const long long stamp = read_number_after( line, "\"cp\":" );
                if( stamp >= 0 ) {
                    self->send_raw( "{\"t\":\"pong\",\"cp\":" + std::to_string( stamp ) + "}\n" );
                }
            };
            c->on_close = [this]( const std::shared_ptr<connection> &self, const std::string & why ) {
                peers.erase( self->id );
                peer_count = static_cast<int>( peers.size() );
                push_event( event::kind::closed, self->id, why );
            };
            peers[c->id] = c;
            peer_count = static_cast<int>( peers.size() );
            push_event( event::kind::opened, c->id, addr );
            c->start();
            do_accept();
        } );
    }

    void arm_heartbeat() {
        hb_timer.expires_after( std::chrono::milliseconds( heartbeat_interval_ms ) );
        hb_timer.async_wait( [this]( const asio::error_code & ec ) {
            if( ec ) {
                return;
            }
            const int64_t now = now_ms();
            std::vector<std::shared_ptr<connection>> snapshot;
            snapshot.reserve( peers.size() );
            for( auto &p : peers ) {
                snapshot.push_back( p.second );
            }
            for( auto &c : snapshot ) {
                netlog( "host hb check conn=" + std::to_string( c->id ) + " silent_ms=" +
                        std::to_string( now - c->last_recv_ms ) );
                if( now - c->last_recv_ms > stall_timeout_ms ) {
                    c->close( "peer stopped responding" );
                } else {
                    c->send_raw( "{\"t\":\"hb\"}\n" );
                }
            }
            arm_heartbeat();
        } );
    }
};

std::unique_ptr<host_impl> g_host;

// ---- client -------------------------------------------------------------

struct client_impl {
    asio::io_context io;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> guard;
    std::shared_ptr<connection> conn;
    asio::steady_timer hb_timer{ io };
    std::thread thread;
    std::atomic<bool> open{ false };
    std::atomic<int> rtt{ -1 };
    std::atomic<int64_t> ping_stamp{ -1 };

    ~client_impl() {
        if( guard ) {
            guard->reset();
        }
        io.stop();
        if( thread.joinable() ) {
            thread.join();
        }
    }

    void arm_heartbeat() {
        hb_timer.expires_after( std::chrono::milliseconds( heartbeat_interval_ms ) );
        hb_timer.async_wait( [this]( const asio::error_code & ec ) {
            if( ec || !conn || conn->closed ) {
                return;
            }
            const int64_t now = now_ms();
            if( now - conn->last_recv_ms > stall_timeout_ms ) {
                conn->close( "host stopped responding" );
                return;
            }
            ping_stamp = now;
            netlog( "client hb" );
            conn->send_raw( "{\"t\":\"hb\",\"cp\":" + std::to_string( now ) + "}\n" );
            arm_heartbeat();
        } );
    }
};

std::unique_ptr<client_impl> g_client;

} // namespace

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch() ).count();
}

bool host_listen( uint16_t port, std::string &err )
{
    host_shutdown();
    auto h = std::make_unique<host_impl>();
    try {
        const tcp::endpoint ep( tcp::v4(), port );
        h->acceptor.open( ep.protocol() );
        h->acceptor.set_option( tcp::acceptor::reuse_address( true ) );
        h->acceptor.bind( ep );
        h->acceptor.listen();
    } catch( const asio::system_error &e ) {
        err = describe( e.code() );
        return false;
    } catch( const std::exception &e ) {
        err = e.what();
        return false;
    }
    h->port = port;
    h->do_accept();
    h->arm_heartbeat();
    host_impl *raw = h.get();
    h->thread = std::thread( [raw]() {
        try {
            raw->io.run();
        } catch( const std::exception &e ) {
            push_event( event::kind::closed, -1, std::string( "network thread error: " ) + e.what() );
        }
    } );
    g_host = std::move( h );
    return true;
}

void host_shutdown()
{
    if( !g_host ) {
        return;
    }
    host_impl *raw = g_host.get();
    asio::post( raw->io, [raw]() {
        asio::error_code ec;
        raw->acceptor.close( ec );
        raw->hb_timer.cancel();
        std::vector<std::shared_ptr<connection>> snapshot;
        for( auto &p : raw->peers ) {
            snapshot.push_back( p.second );
        }
        for( auto &c : snapshot ) {
            c->close( "host closed the session" );
        }
    } );
    // Give the io thread a moment to flush the goodbye packets before stopping.
    const int64_t deadline = now_ms() + 500;
    while( raw->peer_count > 0 && now_ms() < deadline ) {
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }
    g_host.reset();
}

bool host_listening()
{
    return g_host != nullptr;
}

uint16_t host_port()
{
    return g_host ? g_host->port : 0;
}

void host_send( int peer, const std::string &msg )
{
    if( !g_host ) {
        return;
    }
    host_impl *raw = g_host.get();
    asio::post( raw->io, [raw, peer, framed = compress_frame( msg )]() mutable {
        const auto it = raw->peers.find( peer );
        if( it != raw->peers.end() ) {
            it->second->send_raw( std::move( framed ) );
        }
    } );
}

void host_close_peer( int peer )
{
    if( !g_host ) {
        return;
    }
    host_impl *raw = g_host.get();
    asio::post( raw->io, [raw, peer]() {
        const auto it = raw->peers.find( peer );
        if( it != raw->peers.end() ) {
            // Close after pending writes had a chance to go out.
            auto c = it->second;
            auto t = std::make_shared<asio::steady_timer>( raw->io );
            t->expires_after( std::chrono::milliseconds( 200 ) );
            t->async_wait( [c, t]( const asio::error_code & ) {
                c->close( "closed by host" );
            } );
        }
    } );
}

int host_peer_count()
{
    return g_host ? g_host->peer_count.load() : 0;
}

bool client_connect( const std::string &host, uint16_t port, int timeout_ms, std::string &err )
{
    client_disconnect();
    auto c = std::make_unique<client_impl>();
    try {
        tcp::resolver resolver( c->io );
        asio::error_code rec;
        const auto endpoints = resolver.resolve( host, std::to_string( port ), rec );
        if( rec ) {
            err = "cannot resolve address: " + describe( rec );
            return false;
        }
        tcp::socket sock( c->io );
        bool finished = false;
        asio::error_code connect_ec = asio::error::timed_out;
        asio::steady_timer deadline( c->io );
        asio::async_connect( sock, endpoints,
        [&]( const asio::error_code & ec, const tcp::endpoint & ) {
            finished = true;
            connect_ec = ec;
            deadline.cancel();
        } );
        deadline.expires_after( std::chrono::milliseconds( timeout_ms ) );
        deadline.async_wait( [&]( const asio::error_code & ec ) {
            if( !ec && !finished ) {
                asio::error_code ignore;
                sock.close( ignore );
            }
        } );
        c->io.run();
        c->io.restart();
        if( !finished || connect_ec ) {
            err = finished ? describe( connect_ec ) : std::string( "connection timed out" );
            return false;
        }
        c->conn = std::make_shared<connection>( std::move( sock ) );
    } catch( const std::exception &e ) {
        err = e.what();
        return false;
    }

    client_impl *raw = c.get();
    c->conn->on_line = []( const std::shared_ptr<connection> &, std::string & line ) {
        push_event( event::kind::line, 0, std::move( line ) );
    };
    c->conn->on_heartbeat = [raw]( const std::shared_ptr<connection> &, const std::string & line ) {
        if( line.find( "\"t\":\"pong\"" ) != std::string::npos ) {
            const long long stamp = read_number_after( line, "\"cp\":" );
            if( stamp >= 0 && stamp == raw->ping_stamp.load() ) {
                raw->rtt = static_cast<int>( now_ms() - stamp );
            }
        }
    };
    c->conn->on_close = [raw]( const std::shared_ptr<connection> &, const std::string & why ) {
        raw->open = false;
        raw->hb_timer.cancel();
        push_event( event::kind::closed, 0, why );
    };
    c->guard.emplace( asio::make_work_guard( c->io ) );
    c->open = true;
    asio::post( c->io, [raw]() {
        raw->conn->start();
        raw->arm_heartbeat();
    } );
    c->thread = std::thread( [raw]() {
        try {
            raw->io.run();
        } catch( const std::exception &e ) {
            raw->open = false;
            push_event( event::kind::closed, 0, std::string( "network thread error: " ) + e.what() );
        }
    } );
    g_client = std::move( c );
    return true;
}

void client_disconnect()
{
    if( !g_client ) {
        return;
    }
    client_impl *raw = g_client.get();
    raw->open = false;
    asio::post( raw->io, [raw]() {
        if( raw->conn ) {
            // Silence the close callback: a deliberate disconnect is not news.
            raw->conn->on_close = nullptr;
            raw->conn->close( "disconnected" );
        }
        raw->hb_timer.cancel();
        if( raw->guard ) {
            raw->guard->reset();
        }
    } );
    // Let a final queued message (e.g. "quit") reach the wire.
    std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
    g_client.reset();
}

bool client_connected()
{
    return g_client && g_client->open.load();
}

void client_send( const std::string &msg )
{
    if( !g_client || !g_client->open.load() ) {
        return;
    }
    client_impl *raw = g_client.get();
    asio::post( raw->io, [raw, framed = compress_frame( msg )]() mutable {
        if( raw->conn ) {
            raw->conn->send_raw( std::move( framed ) );
        }
    } );
}

int client_rtt_ms()
{
    return g_client ? g_client->rtt.load() : -1;
}

bool poll( event &out )
{
    std::lock_guard<std::mutex> lk( events_mutex );
    if( events.empty() ) {
        return false;
    }
    out = std::move( events.front() );
    events.pop_front();
    return true;
}

void clear_events()
{
    std::lock_guard<std::mutex> lk( events_mutex );
    events.clear();
}

namespace
{
// Tailscale (100.64/10), Radmin (26/8) and Hamachi (25/8) addresses are the
// ones a remote partner most likely uses, so list them first.
bool ip_is_vpn_like( const std::string &ip )
{
    int a = 0;
    int b = 0;
    if( std::sscanf( ip.c_str(), "%d.%d", &a, &b ) < 2 ) {
        return false;
    }
    return a == 26 || a == 25 || ( a == 100 && b >= 64 && b <= 127 );
}
} // namespace

std::vector<std::string> local_ipv4s()
{
    std::vector<std::string> result;
    const auto add = [&result]( const std::string & ip ) {
        if( ip.empty() || ip == "0.0.0.0" || ip.rfind( "127.", 0 ) == 0 ||
            ip.rfind( "169.254.", 0 ) == 0 ) {
            return;
        }
        if( std::find( result.begin(), result.end(), ip ) == result.end() ) {
            result.push_back( ip );
        }
    };
#if defined(_WIN32)
    ULONG buf_len = 32768;
    std::vector<char> buf( buf_len );
    auto *addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES *>( buf.data() );
    if( GetAdaptersAddresses( AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                              GAA_FLAG_SKIP_DNS_SERVER, nullptr, addrs, &buf_len ) == NO_ERROR ) {
        for( auto *a = addrs; a != nullptr; a = a->Next ) {
            if( a->OperStatus != IfOperStatusUp ) {
                continue;
            }
            for( auto *u = a->FirstUnicastAddress; u != nullptr; u = u->Next ) {
                auto *sa = reinterpret_cast<sockaddr_in *>( u->Address.lpSockaddr );
                char ipbuf[INET_ADDRSTRLEN] = {};
                inet_ntop( AF_INET, &sa->sin_addr, ipbuf, sizeof( ipbuf ) );
                add( ipbuf );
            }
        }
    }
#else
    ifaddrs *ifs = nullptr;
    if( getifaddrs( &ifs ) == 0 ) {
        for( ifaddrs *ifa = ifs; ifa != nullptr; ifa = ifa->ifa_next ) {
            if( ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET ||
                !( ifa->ifa_flags & IFF_UP ) || ( ifa->ifa_flags & IFF_LOOPBACK ) ) {
                continue;
            }
            auto *sa = reinterpret_cast<sockaddr_in *>( ifa->ifa_addr );
            char ipbuf[INET_ADDRSTRLEN] = {};
            inet_ntop( AF_INET, &sa->sin_addr, ipbuf, sizeof( ipbuf ) );
            add( ipbuf );
        }
        freeifaddrs( ifs );
    }
#endif
    std::stable_sort( result.begin(), result.end(), []( const std::string & x,
    const std::string & y ) {
        return ip_is_vpn_like( x ) && !ip_is_vpn_like( y );
    } );
    return result;
}

} // namespace cata_mp::net
