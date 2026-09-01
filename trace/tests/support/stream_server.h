#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

namespace trace::testing {

/// A one-connection HTTP server that plays a file out at roughly real time.
///
/// ## Why the camera tests bring their own server
///
/// The capture path is only worth testing against a real socket. A test that
/// pointed CaptureSession at a file on disk would exercise none of what makes
/// live capture different: the network open, packets arriving slower than they
/// can be written, a stream that ends when the other end says so rather than at
/// a known byte offset, and — the case that matters most — a link that drops in
/// the middle.
///
/// The machine these tests were written on has no camera, and CI has none
/// either. Spawning `ffmpeg` as a server would work on a developer's machine
/// and skip everywhere else, which for the capture path means never running.
/// Serving the bytes from the test process instead costs a hundred lines and
/// makes the test run everywhere, including on Windows.
///
/// What this is faithful to: HTTP-served MPEG over TCP is a real IP-camera
/// transport, and it reaches TRACE through exactly the same avformat call as
/// RTSP. What it is not: the RTSP handshake itself, which has no server
/// available here to test against. `docs/CAMERA_INGEST.md` says so plainly
/// rather than implying the whole transport matrix has been exercised.
/// How the served stream behaves. At namespace scope rather than nested inside
/// StreamServer because a nested class's default member initialisers are not
/// usable from a default argument of the enclosing class until that class is
/// complete, which GCC enforces.
struct StreamOptions {
    /// Bytes handed to the socket at a time.
    std::size_t chunkBytes = 8 * 1024;
    /// Pause between chunks, which is what makes the source behave like a
    /// camera rather than a file being read at disk speed.
    int chunkDelayMs = 8;
    /// Close the connection after this many bytes, mid-stream, without
    /// finishing the response. Zero serves the whole file. This is how a camera
    /// dropping off the network is reproduced deterministically.
    std::size_t dropAfterBytes = 0;
    /// Serve the file again to a second connection. Used by the reconnect test:
    /// a camera that comes back is a camera that answers twice.
    bool serveAgainAfterDrop = false;
};

class StreamServer {
public:
    using Options = StreamOptions;

    StreamServer();
    ~StreamServer();
    StreamServer(const StreamServer&) = delete;
    StreamServer& operator=(const StreamServer&) = delete;

    /// Binds to an ephemeral port on the loopback interface and starts serving.
    /// Returns false when a socket could not be opened, which a test reports
    /// rather than treating as a capture failure.
    bool start(const std::filesystem::path& file, const StreamOptions& options = {});
    void stop();

    /// The URL to point a capture at. Empty until `start` succeeds.
    const std::string& url() const { return url_; }
    /// How many connections were accepted. The reconnect test asserts on this:
    /// a capture that reported a gap and then more video must have reconnected.
    int connections() const { return connections_.load(std::memory_order_relaxed); }

private:
    void serve(std::filesystem::path file, StreamOptions options);

    std::thread worker_;
    std::atomic<bool> stopping_{false};
    std::atomic<int> connections_{0};
    std::string url_;
    std::int64_t listener_ = -1;  ///< SOCKET on Windows, fd elsewhere; -1 when closed
};

}  // namespace trace::testing
