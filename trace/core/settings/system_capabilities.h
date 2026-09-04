#pragma once

#include <cstdint>
#include <string>

namespace trace {

/// What the workstation actually offers. Detected, never assumed: the settings
/// screen shows these so an operator can see why an option is unavailable.
struct SystemCapabilities {
    unsigned int logicalCores = 0;
    std::int64_t totalMemoryBytes = 0;     ///< -1 when the platform did not report it
    std::int64_t dataDiskFreeBytes = 0;    ///< free space on the data directory's volume
    std::int64_t dataDiskTotalBytes = 0;
    std::string operatingSystem;
    std::string architecture;
};

SystemCapabilities detectSystemCapabilities(const std::string& dataDirectory);

}  // namespace trace
