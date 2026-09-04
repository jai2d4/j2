#include "core/settings/system_capabilities.h"

#include <filesystem>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace trace {

SystemCapabilities detectSystemCapabilities(const std::string& dataDirectory) {
    SystemCapabilities capabilities;
    capabilities.logicalCores = std::thread::hardware_concurrency();

#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) != 0) {
        capabilities.totalMemoryBytes = static_cast<std::int64_t>(status.ullTotalPhys);
    } else {
        capabilities.totalMemoryBytes = -1;
    }
    capabilities.operatingSystem = "Windows";
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGE_SIZE);
    capabilities.totalMemoryBytes =
        (pages > 0 && pageSize > 0) ? static_cast<std::int64_t>(pages) * pageSize : -1;

    struct utsname info {};
    if (uname(&info) == 0) {
        capabilities.operatingSystem = std::string(info.sysname) + " " + info.release;
        capabilities.architecture = info.machine;
    } else {
        capabilities.operatingSystem = "POSIX";
    }
#endif

#if defined(_WIN32)
#if defined(_M_X64)
    capabilities.architecture = "x86_64";
#elif defined(_M_ARM64)
    capabilities.architecture = "arm64";
#endif
#endif

    std::error_code ec;
    if (!dataDirectory.empty()) {
        const auto space = std::filesystem::space(std::filesystem::path(dataDirectory), ec);
        if (!ec) {
            capabilities.dataDiskFreeBytes = static_cast<std::int64_t>(space.available);
            capabilities.dataDiskTotalBytes = static_cast<std::int64_t>(space.capacity);
        }
    }
    return capabilities;
}

}  // namespace trace
