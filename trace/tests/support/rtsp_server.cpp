#include "tests/support/rtsp_server.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

#if defined(_WIN32)
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
    std::size_t sent = 0;
    while (sent < length) {
        const int wrote = ::send(s, data + sent, static_cast<int>(length - sent), 0);
        if (wrote <= 0) return -1;
        sent += static_cast<std::size_t>(wrote);
    }
    return static_cast<int>(sent);
}
struct WinsockOnce {
    WinsockOnce() { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); }
};
void ensureWinsock() { static WinsockOnce once; (void)once; }
#else
using Socket = int;
constexpr Socket kInvalid = -1;
void closeSocket(Socket s) { ::close(s); }
int sendAll(Socket s, const char* data, std::size_t length) {
    std::size_t sent = 0;
    while (sent < length) {
        // MSG_NOSIGNAL: the capture hanging up must not kill the test process.
        const auto wrote = ::send(s, data + sent, length - sent, MSG_NOSIGNAL);
        if (wrote <= 0) return -1;
        sent += static_cast<std::size_t>(wrote);
    }
    return static_cast<int>(sent);
}
void ensureWinsock() {}
#endif

/// Everything one served connection needs, so the AVIO write callback can reach
/// the socket without a global.
struct Interleaver {
    Socket client = kInvalid;
    int packetsSent = 0;
    int dropAfter = 0;
    bool broken = false;
};

/// RFC 2326 §10.12 framing: '$', channel, 16-bit big-endian length, payload.
///
/// This is the callback the RTP muxer writes into, so what lands on the wire is
/// FFmpeg's own packetisation rather than anything reassembled here.
int writeInterleaved(void* opaque, std::uint8_t* buffer, int size) {
    auto* state = static_cast<Interleaver*>(opaque);
    if (state->broken || size <= 0) return size;
    if (state->dropAfter > 0 && state->packetsSent >= state->dropAfter) {
        state->broken = true;
        return AVERROR(EPIPE);
    }

    char header[4];
    header[0] = '$';
    header[1] = 0;  // channel 0: RTP for the first stream
    header[2] = static_cast<char>((size >> 8) & 0xFF);
    header[3] = static_cast<char>(size & 0xFF);

    if (sendAll(state->client, header, sizeof(header)) < 0 ||
        sendAll(state->client, reinterpret_cast<const char*>(buffer), static_cast<std::size_t>(size)) < 0) {
        state->broken = true;
        return AVERROR(EPIPE);
    }
    ++state->packetsSent;
    return size;
}

std::string headerValue(const std::string& request, const std::string& name) {
    // Case-insensitive field lookup, line-oriented.
    std::string lowered = request;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string needle = name;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    needle += ":";

    const auto at = lowered.find(needle);
    if (at == std::string::npos) return {};
    const auto start = at + needle.size();
    const auto end = request.find("\r\n", start);
    std::string value = request.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    return value.substr(first);
}

std::string response(const std::string& sequence, const std::string& extra = {},
                     const std::string& body = {}) {
    std::string out = "RTSP/1.0 200 OK\r\nCSeq: " + sequence + "\r\n";
    out += extra;
    if (!body.empty()) {
        out += "Content-Type: application/sdp\r\nContent-Length: " +
               std::to_string(body.size()) + "\r\n";
    }
    out += "\r\n";
    out += body;
    return out;
}

}  // namespace

RtspServer::RtspServer() = default;
RtspServer::~RtspServer() { stop(); }

bool RtspServer::start(const std::filesystem::path& file, const RtspOptions& options) {
    ensureWinsock();

    Socket listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener == kInvalid) return false;

    const int reuse = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                 sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    // Loopback only: a test must never open a listening port to the network it
    // happens to be running on.
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

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
    url_ = "rtsp://127.0.0.1:" + std::to_string(ntohs(bound.sin_port)) + "/live";
    stopping_.store(false, std::memory_order_relaxed);
    worker_ = std::thread(&RtspServer::serve, this, file, options);
    return true;
}

void RtspServer::stop() {
    stopping_.store(true, std::memory_order_relaxed);
    if (listener_ >= 0) {
        closeSocket(static_cast<Socket>(listener_));
        listener_ = -1;
    }
    if (worker_.joinable()) worker_.join();
}

void RtspServer::serve(std::filesystem::path file, RtspOptions options) {
    if (listener_ < 0) return;

    Socket client = ::accept(static_cast<Socket>(listener_), nullptr, nullptr);
    if (client == kInvalid) return;  // stop() closed the listener
    connections_.fetch_add(1, std::memory_order_relaxed);

    const int noDelay = 1;
    ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay),
                 sizeof(noDelay));

    // ------------------------------------------------------------- source
    AVFormatContext* input = nullptr;
    if (avformat_open_input(&input, file.string().c_str(), nullptr, nullptr) < 0) {
        closeSocket(client);
        return;
    }
    if (avformat_find_stream_info(input, nullptr) < 0) {
        avformat_close_input(&input);
        closeSocket(client);
        return;
    }
    const int videoStream = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStream < 0) {
        avformat_close_input(&input);
        closeSocket(client);
        return;
    }

    // -------------------------------------------------------- RTP muxer
    //
    // A real RTP muxer, so the SDP and every packet on the wire come from
    // FFmpeg rather than from a template written to satisfy one client.
    Interleaver state;
    state.client = client;
    state.dropAfter = options.dropAfterPackets;

    AVFormatContext* rtp = nullptr;
    avformat_alloc_output_context2(&rtp, nullptr, "rtp", nullptr);
    if (rtp == nullptr) {
        avformat_close_input(&input);
        closeSocket(client);
        return;
    }

    AVStream* out = avformat_new_stream(rtp, nullptr);
    if (out == nullptr ||
        avcodec_parameters_copy(out->codecpar, input->streams[videoStream]->codecpar) < 0) {
        avformat_free_context(rtp);
        avformat_close_input(&input);
        closeSocket(client);
        return;
    }
    out->codecpar->codec_tag = 0;
    out->time_base = input->streams[videoStream]->time_base;

    constexpr int kPacketSize = 1400;
    auto* ioBuffer = static_cast<std::uint8_t*>(av_malloc(kPacketSize));
    AVIOContext* pb =
        avio_alloc_context(ioBuffer, kPacketSize, 1, &state, nullptr, writeInterleaved, nullptr);
    // Tells the muxer to emit one RTP packet per write, which is what makes the
    // interleaving above one-packet-per-frame-header rather than a byte stream.
    pb->max_packet_size = kPacketSize;
    rtp->pb = pb;
    rtp->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (avformat_write_header(rtp, nullptr) < 0) {
        avio_context_free(&pb);
        av_free(ioBuffer);
        avformat_free_context(rtp);
        avformat_close_input(&input);
        closeSocket(client);
        return;
    }

    char sdpBuffer[4096] = {};
    AVFormatContext* contexts[1] = {rtp};
    av_sdp_create(contexts, 1, sdpBuffer, sizeof(sdpBuffer));
    std::string sdp(sdpBuffer);

    // ------------------------------------------------------- signalling
    std::string pending;
    bool playing = false;
    std::vector<char> buffer(8192);

    while (!playing && !stopping_.load(std::memory_order_relaxed)) {
        const auto received = ::recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received <= 0) break;
        pending.append(buffer.data(), static_cast<std::size_t>(received));

        // One request per iteration, so a client that pipelines is still served
        // message by message.
        std::size_t end;
        while ((end = pending.find("\r\n\r\n")) != std::string::npos) {
            const std::string request = pending.substr(0, end + 4);
            pending.erase(0, end + 4);

            const std::string sequence = headerValue(request, "CSeq");
            std::string reply;

            if (request.rfind("OPTIONS", 0) == 0) {
                reply = response(sequence,
                                 "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n");
            } else if (request.rfind("DESCRIBE", 0) == 0) {
                sawDescribe_.store(true, std::memory_order_relaxed);
                reply = response(sequence, "Content-Base: " + url_ + "/\r\n", sdp);
            } else if (request.rfind("SETUP", 0) == 0) {
                sawSetup_.store(true, std::memory_order_relaxed);
                // Echo the interleaved channels back. TRACE asks for TCP
                // explicitly (rtsp_transport=tcp), which is the only transport
                // this serves.
                reply = response(sequence,
                                 "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
                                 "Session: 12345678\r\n");
            } else if (request.rfind("PLAY", 0) == 0) {
                sawPlay_.store(true, std::memory_order_relaxed);
                reply = response(sequence, "Session: 12345678\r\nRange: npt=0.000-\r\n");
                playing = true;
            } else if (request.rfind("TEARDOWN", 0) == 0) {
                reply = response(sequence, "Session: 12345678\r\n");
            } else {
                reply = "RTSP/1.0 501 Not Implemented\r\nCSeq: " + sequence + "\r\n\r\n";
            }

            if (sendAll(client, reply.data(), reply.size()) < 0) {
                playing = false;
                break;
            }
            if (playing) break;
        }
    }

    // ------------------------------------------------------------ stream
    if (playing) {
        AVPacket* packet = av_packet_alloc();
        while (packet != nullptr && !stopping_.load(std::memory_order_relaxed) && !state.broken) {
            if (av_read_frame(input, packet) < 0) break;
            if (packet->stream_index != videoStream) {
                av_packet_unref(packet);
                continue;
            }
            av_packet_rescale_ts(packet, input->streams[videoStream]->time_base, out->time_base);
            packet->stream_index = 0;
            if (av_write_frame(rtp, packet) < 0) {
                av_packet_unref(packet);
                break;
            }
            av_packet_unref(packet);
            if (options.packetDelayMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(options.packetDelayMs));
            }
        }
        if (packet != nullptr) av_packet_free(&packet);
        if (!state.broken) av_write_trailer(rtp);
    }

    avio_context_free(&pb);
    av_free(ioBuffer);
    avformat_free_context(rtp);
    avformat_close_input(&input);
    closeSocket(client);
}

}  // namespace trace::testing
