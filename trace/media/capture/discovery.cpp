#include "media/capture/discovery.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "core/common/logging.h"
#include "core/common/uuid.h"

namespace trace::discovery {
namespace {

constexpr const char* kComponent = "discovery";
constexpr const char* kMulticastAddress = "239.255.255.250";
constexpr int kMulticastPort = 3702;

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
void closeSocket(SocketHandle s) { closesocket(s); }
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
void closeSocket(SocketHandle s) { ::close(s); }
#endif

/// The WS-Discovery probe an ONVIF camera answers.
///
/// Written out rather than built with an XML library: it is a fixed document
/// with one variable in it, and adding a SOAP dependency to send eight hundred
/// bytes would be the larger cost. The MessageID must be unique per probe or
/// cameras may treat a repeat as a duplicate and stay silent.
std::string probeMessage(const std::string& messageId) {
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
        "xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
        "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
        "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
        "<e:Header>"
        "<w:MessageID>uuid:" + messageId + "</w:MessageID>"
        "<w:To e:mustUnderstand=\"true\">urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>"
        "<w:Action e:mustUnderstand=\"true\">"
        "http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action>"
        "</e:Header>"
        "<e:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></e:Body>"
        "</e:Envelope>";
}

/// Pulls the values of every occurrence of one tag out of a SOAP reply.
///
/// A deliberately small scan rather than an XML parser. The alternative is a
/// parser dependency for one field of one message, and the failure mode here is
/// benign: a reply this does not understand yields no address and the camera is
/// simply not listed, which is the same outcome as not answering. Nothing is
/// executed or trusted from it — the extracted text is validated as a URI by
/// cameraSourceFromUri before it becomes a CameraSource.
std::vector<std::string> extractTagValues(const std::string& xml, const std::string& localName) {
    std::vector<std::string> values;
    std::size_t at = 0;
    while (at < xml.size()) {
        // Match on the local name so any namespace prefix works.
        const auto open = xml.find(localName + ">", at);
        if (open == std::string::npos) break;
        const auto valueStart = open + localName.size() + 1;
        const auto close = xml.find("</", valueStart);
        if (close == std::string::npos) break;
        values.push_back(xml.substr(valueStart, close - valueStart));
        at = close + 2;
    }
    return values;
}

std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

/// XAddrs is a space-separated list of service URLs. The first http(s) one is
/// the device service; that is the address an operator needs.
std::string firstServiceAddress(const std::string& xaddrs) {
    std::size_t at = 0;
    while (at < xaddrs.size()) {
        const auto next = xaddrs.find(' ', at);
        const std::string candidate =
            trimmed(xaddrs.substr(at, next == std::string::npos ? std::string::npos : next - at));
        if (candidate.rfind("http://", 0) == 0 || candidate.rfind("https://", 0) == 0) {
            return candidate;
        }
        if (next == std::string::npos) break;
        at = next + 1;
    }
    return {};
}

/// Scopes carry onvif://www.onvif.org/name/… and /hardware/… entries.
std::string scopeValue(const std::string& scopes, const std::string& key) {
    const std::string needle = "/" + key + "/";
    const auto at = scopes.find(needle);
    if (at == std::string::npos) return {};
    const auto start = at + needle.size();
    const auto end = scopes.find_first_of(" \t\r\n", start);
    std::string value = scopes.substr(start, end == std::string::npos ? std::string::npos : end - start);
    // Scope values are percent-encoded; only the space matters in practice.
    std::size_t plus = 0;
    while ((plus = value.find("%20", plus)) != std::string::npos) {
        value.replace(plus, 3, " ");
        ++plus;
    }
    return value;
}

}  // namespace

Result<std::vector<CameraSource>> findNetworkCameras(int timeoutMs) {
    using ResultType = Result<std::vector<CameraSource>>;
    std::vector<CameraSource> found;

#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return ResultType::failure(ErrorCode::Internal, "Unable to start Windows sockets");
    }
    struct WsaGuard {
        ~WsaGuard() { WSACleanup(); }
    } wsaGuard;
#endif

    SocketHandle handle = socket(AF_INET, SOCK_DGRAM, 0);
    if (handle == kInvalidSocket) {
        return ResultType::failure(ErrorCode::Internal, "Unable to open a discovery socket");
    }
    struct SocketGuard {
        SocketHandle handle;
        ~SocketGuard() { closeSocket(handle); }
    } guard{handle};

    // Multicast TTL of 1: discovery is for the local segment. A higher value
    // would send probes to networks the operator did not intend to scan, which
    // on a shared corporate network is not a neutral act.
    const int ttl = 1;
    setsockopt(handle, IPPROTO_IP, IP_MULTICAST_TTL,
               reinterpret_cast<const char*>(&ttl), sizeof(ttl));

#if defined(_WIN32)
    DWORD timeout = static_cast<DWORD>(timeoutMs);
    setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif

    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(static_cast<std::uint16_t>(kMulticastPort));
    if (inet_pton(AF_INET, kMulticastAddress, &destination.sin_addr) != 1) {
        return ResultType::failure(ErrorCode::Internal, "Bad discovery multicast address");
    }

    const std::string probe = probeMessage(generateUuid());
    const auto sent = sendto(handle, probe.data(), static_cast<int>(probe.size()), 0,
                             reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
    if (sent < 0) {
        // No route to the multicast group: a machine with no network, or one
        // where it is blocked. Reported rather than returned as "no cameras",
        // because those need different things from an operator.
        return ResultType::failure(
            ErrorCode::IoError, "Could not send a discovery probe to the local network",
            "The network may be unavailable, or multicast may be blocked between here and the "
            "cameras. A camera can still be added by address.");
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::vector<char> buffer(65536);

    while (std::chrono::steady_clock::now() < deadline) {
        sockaddr_in from{};
#if defined(_WIN32)
        int fromLength = sizeof(from);
#else
        socklen_t fromLength = sizeof(from);
#endif
        const auto received = recvfrom(handle, buffer.data(), static_cast<int>(buffer.size()), 0,
                                       reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (received <= 0) break;  // timed out; every camera that was going to answer has

        const std::string reply(buffer.data(), static_cast<std::size_t>(received));
        for (const std::string& xaddrs : extractTagValues(reply, "XAddrs")) {
            const std::string service = firstServiceAddress(xaddrs);
            if (service.empty()) continue;

            auto parsed = cameraSourceFromUri(service);
            if (!parsed) continue;
            CameraSource camera = parsed.take();

            const auto scopes = extractTagValues(reply, "Scopes");
            if (!scopes.empty()) {
                const std::string name = scopeValue(scopes.front(), "name");
                const std::string hardware = scopeValue(scopes.front(), "hardware");
                if (!name.empty()) camera.name = name;
                if (!hardware.empty()) camera.model = hardware;
            }
            // Answering a multicast probe says the camera is on this segment; it
            // says nothing about whether the last hop was a cable or WiFi, so
            // the link stays unknown rather than being guessed.
            camera.link = CameraLink::Unknown;

            const bool duplicate =
                std::any_of(found.begin(), found.end(),
                            [&camera](const CameraSource& seen) { return seen.id == camera.id; });
            if (!duplicate) found.push_back(std::move(camera));
        }
    }

    logInfo(kComponent, "ONVIF discovery finished",
            JsonValue::object().set("cameras", static_cast<std::int64_t>(found.size())));
    return ResultType::success(std::move(found));
}

Result<std::vector<CameraSource>> findLocalDevices() {
    using ResultType = Result<std::vector<CameraSource>>;
    std::vector<CameraSource> found;

#if defined(_WIN32)
    // DirectShow enumeration needs libavdevice's own listing, which has no
    // programmatic API — it writes to the log. Rather than parse that, Windows
    // takes attached cameras by explicit name for now. Said plainly instead of
    // returning an empty list that looks like "no cameras attached".
    return ResultType::failure(
        ErrorCode::Unsupported, "Listing attached cameras is not implemented on Windows",
        "Add the camera by name, as video=<device name>. Network cameras are unaffected.");
#else
    std::error_code ec;
    if (!std::filesystem::exists("/dev", ec)) return ResultType::success(std::move(found));

    std::vector<std::filesystem::path> nodes;
    for (const auto& entry : std::filesystem::directory_iterator("/dev", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("video", 0) == 0) nodes.push_back(entry.path());
    }
    // /dev/video0, /dev/video1 … in numeric order rather than lexicographic, so
    // video10 does not sort between video1 and video2.
    std::sort(nodes.begin(), nodes.end(), [](const auto& a, const auto& b) {
        const std::string an = a.filename().string().substr(5);
        const std::string bn = b.filename().string().substr(5);
        if (an.size() != bn.size()) return an.size() < bn.size();
        return an < bn;
    });

    for (const auto& node : nodes) {
        auto parsed = cameraSourceFromUri(node.string());
        if (!parsed) continue;
        CameraSource camera = parsed.take();
        camera.name = node.string();
        found.push_back(std::move(camera));
    }

    // A node existing does not mean it is a capture device — many /dev/video*
    // entries on Linux are metadata or output nodes belonging to a camera whose
    // capture node is a different number. They are listed as candidates and
    // probeCamera decides; filtering here would need V4L2 ioctls for a result
    // that opening the device answers directly.
    logInfo(kComponent, "Local capture devices enumerated",
            JsonValue::object().set("devices", static_cast<std::int64_t>(found.size())));
    return ResultType::success(std::move(found));
#endif
}

Result<Outcome> findCameras(const Options& options) {
    using ResultType = Result<Outcome>;
    Outcome outcome;

    if (options.includeLocalDevices) {
        if (auto local = findLocalDevices(); local) {
            for (auto& camera : local.take()) outcome.cameras.push_back(std::move(camera));
        } else {
            outcome.unavailable.emplace_back("attached devices", local.error().message());
        }
    }

    if (options.includeNetworkCameras) {
        if (auto network = findNetworkCameras(options.networkTimeoutMs); network) {
            for (auto& camera : network.take()) outcome.cameras.push_back(std::move(camera));
        } else {
            outcome.unavailable.emplace_back("network cameras", network.error().message());
        }
    }

    return ResultType::success(std::move(outcome));
}

}  // namespace trace::discovery
