#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace trace {

/// A marked moment in an evidence item.
///
/// The authoritative position is `timestampUs` — the presentation timestamp of
/// the media. `frameNumber` is recorded only when the decoder could establish a
/// reliable frame index, and is display information rather than identity.
struct Bookmark {
    std::string id;
    std::string caseId;
    std::string evidenceId;
    std::int64_t timestampUs = 0;
    std::optional<std::int64_t> frameNumber;
    std::string title;
    std::string note;
    std::optional<std::string> colour;
    std::string createdBy;
    std::string createdAt;
    std::string modifiedAt;
};

}  // namespace trace
