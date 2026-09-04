#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

namespace trace::testing {

/// A minimal RTSP server, so the RTSP path can actually be tested.
///
/// ## Why this exists
///
/// `docs/CAMERA_INGEST.md` §0 listed the RTSP handshake as **not executed**:
/// every capture test reached avformat through an HTTP socket instead, which
/// exercises everything downstream of the handshake and none of the handshake
/// itself. That was an honest gap and it stayed open only because no RTSP
/// server was available here — FFmpeg's RTSP muxer in this build has no listen
/// mode, and no standalone server is installed.
///
/// So this is one. It is small because it only has to be correct for one
/// client: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, and RTP interleaved back
/// over the same TCP connection.
///
/// ## What is real about it
///
/// The parts that matter are not simulated:
///
/// - The **SDP** comes from `av_sdp_create` over a real RTP muxer, not from a
///   handwritten template that happens to satisfy this one client.
/// - The **RTP packets** are produced by FFmpeg's own `rtp` muxer, which means
///   the payload type, timestamps, sequence numbers and H.264 NAL
///   fragmentation are the library's, not this file's guesses.
/// - The **interleaving** is RFC 2326 §10.12 framing: `$`, channel, 16-bit
///   length, then the packet, on the same socket the control messages used.
///
/// What that leaves TRACE doing is exactly what it does against a real camera:
/// `avformat_open_input` on an `rtsp://` URL with `rtsp_transport=tcp`,
/// negotiating a session and reading interleaved RTP.
///
/// ## What it is not
///
/// Not a general RTSP server. It serves one stream to one client over TCP, has
/// no authentication, no UDP transport, no RTCP, and no seeking. A camera that
/// needed any of those would not be tested by this. It closes the gap it was
/// written for and does not pretend to be broader.
/// How the served stream behaves. At namespace scope rather than nested inside
/// RtspServer because a nested class's default member initialisers are not
/// usable from a default argument of the enclosing class until that class is
/// complete, which GCC enforces. StreamOptions is shaped the same way and for
/// the same reason.
struct RtspOptions {
    /// Pause between packets, so the source behaves like a camera rather than a
    /// file read at disk speed.
    int packetDelayMs = 4;
    /// Stop sending after this many RTP packets and close the connection,
    /// mid-stream. Zero plays to the end. This is how a camera dropping off the
    /// network is reproduced over RTSP specifically.
    int dropAfterPackets = 0;
};

class RtspServer {
public:
    using Options = RtspOptions;

    RtspServer();
    ~RtspServer();
    RtspServer(const RtspServer&) = delete;
    RtspServer& operator=(const RtspServer&) = delete;

    /// Binds to an ephemeral loopback port and starts serving `file`.
    /// Returns false when a socket could not be opened.
    bool start(const std::filesystem::path& file, const RtspOptions& options = {});
    void stop();

    /// The rtsp:// URL to point a capture at. Empty until `start` succeeds.
    const std::string& url() const { return url_; }

    /// How far the client got. The tests assert on these rather than only on
    /// the recording, because "a file appeared" does not prove the handshake
    /// happened — a server that answered nothing and a server that negotiated
    /// correctly can both leave a capture with zero segments.
    bool sawDescribe() const { return sawDescribe_.load(std::memory_order_relaxed); }
    bool sawSetup() const { return sawSetup_.load(std::memory_order_relaxed); }
    bool sawPlay() const { return sawPlay_.load(std::memory_order_relaxed); }
    int connections() const { return connections_.load(std::memory_order_relaxed); }

private:
    void serve(std::filesystem::path file, RtspOptions options);

    std::thread worker_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> sawDescribe_{false};
    std::atomic<bool> sawSetup_{false};
    std::atomic<bool> sawPlay_{false};
    std::atomic<int> connections_{0};
    std::string url_;
    std::int64_t listener_ = -1;  ///< SOCKET on Windows, fd elsewhere; -1 when closed
};

}  // namespace trace::testing
