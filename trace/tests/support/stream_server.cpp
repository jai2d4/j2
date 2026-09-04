#include "tests/support/stream_server.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <vector>

#if defined(_WIN32)
// Before any Windows header: windows.h defines min and max as macros, which
// turns std::min({a, b, c}) into a syntax error rather than anything readable.
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace trace::testing {
namespace {

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kInvalid = INVALID_SOCKET;
void closeSocket(Socket s) { closesocket(s); }
int sendAll(Socket s, const char* data, std::size_t length) {
    return ::send(s, data, static_cast<int>(length), 0);
}
/// Winsock needs starting once per process; a static does that and never
/// tears it down, which is correct for a test binary.
struct WinsockOnce {
    WinsockOnce() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
};
void ensureWinsock() { static WinsockOnce once; (void)once; }
#else
using Socket = int;
constexpr Socket kInvalid = -1;
void closeSocket(Socket s) { ::close(s); }
int sendAll(Socket s, const char* data, std::size_t length) {
    // MSG_NOSIGNAL: the client hanging up must not take the test process with
    // it via SIGPIPE. That happens on every run where the capture stops first.
    return static_cast<int>(::send(s, data, length, MSG_NOSIGNAL));
}
void ensureWinsock() {}
#endif

}  // namespace

StreamServer::StreamServer() = default;

StreamServer::~StreamServer() { stop(); }

bool StreamServer::start(const std::filesystem::path& file, const StreamOptions& options) {
    ensureWinsock();

    Socket listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener == kInvalid) return false;

    const int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                 sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    // Loopback only. A test must never open a listening port to the network it
    // happens to be running on.
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;  // let the OS pick, so parallel tests cannot collide

    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 2) != 0) {
        closeSocket(listener);
        return false;
    }

    sockaddr_in bound{};
#if defined(_WIN32)
    int boundLength = sizeof(bound);
#else
    socklen_t boundLength = sizeof(bound);
#endif
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &boundLength) != 0) {
        closeSocket(listener);
        return false;
    }

    listener_ = static_cast<std::int64_t>(listener);
    url_ = "http://127.0.0.1:" + std::to_string(ntohs(bound.sin_port)) + "/live";
    stopping_.store(false, std::memory_order_relaxed);
    worker_ = std::thread(&StreamServer::serve, this, file, options);
    return true;
}

void StreamServer::stop() {
    stopping_.store(true, std::memory_order_relaxed);
    if (listener_ >= 0) {
        // Closing the listener is what wakes the blocked accept().
        closeSocket(static_cast<Socket>(listener_));
        listener_ = -1;
    }
    if (worker_.joinable()) worker_.join();
}

void StreamServer::serve(std::filesystem::path file, StreamOptions options) {
    std::vector<char> body;
    {
        std::ifstream in(file, std::ios::binary);
        if (!in) return;
        body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    // No Content-Length: this is a live stream, and a length would tell the
    // demuxer the end is known — which for a camera it is not.
    const std::string header =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: video/mp4\r\n"
        "Connection: close\r\n"
        "\r\n";

    const int rounds = options.serveAgainAfterDrop ? 2 : 1;
    for (int round = 0; round < rounds; ++round) {
        if (stopping_.load(std::memory_order_relaxed) || listener_ < 0) return;

        Socket client = ::accept(static_cast<Socket>(listener_), nullptr, nullptr);
        if (client == kInvalid) return;  // stop() closed the listener
        connections_.fetch_add(1, std::memory_order_relaxed);

        const int noDelay = 1;
        ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay),
                     sizeof(noDelay));

        // Read and discard the request. The demuxer's GET is the only thing that
        // arrives and none of it changes what is served.
        char request[2048];
        (void)::recv(client, request, sizeof(request), 0);

        if (sendAll(client, header.data(), header.size()) < 0) {
            closeSocket(client);
            continue;
        }

        // The first round drops early when asked; a later round always plays to
        // the end, so the reconnect test sees video after the gap.
        const std::size_t dropAt =
            (round == 0 && options.dropAfterBytes > 0) ? options.dropAfterBytes : body.size();

        std::size_t sent = 0;
        while (sent < body.size() && sent < dropAt) {
            if (stopping_.load(std::memory_order_relaxed)) break;
            const std::size_t take =
                std::min({options.chunkBytes, body.size() - sent, dropAt - sent});
            if (sendAll(client, body.data() + sent, take) < 0) break;  // the capture hung up
            sent += take;
            if (options.chunkDelayMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(options.chunkDelayMs));
            }
        }

        // Closing without finishing is exactly what a camera dropping looks
        // like from the far end: no trailer, no length, just the socket going
        // away mid-packet.
        closeSocket(client);
    }
}

}  // namespace trace::testing
