#pragma once

#include <cstdint>
#include <string>

namespace trace {

namespace detail {
/// Draws a fresh seed from the platform entropy source.
///
/// Exposed only so the unit suite can prove seeding is not degenerate: an
/// earlier version wrote seed bytes through an aliasing pointer, which an
/// optimising compiler was entitled to discard — every process then produced
/// the same identifier sequence. Evidence identity depends on this being
/// genuinely unpredictable.
std::uint64_t generateEntropySeed();
}  // namespace detail


/// Random (version 4) UUID generation. Every persisted TRACE entity is
/// identified by one of these; identity never depends on a filename or on a
/// display number that a user might edit.
std::string generateUuid();

/// True when `text` is a canonical 8-4-4-4-12 lowercase-or-uppercase UUID.
bool isUuid(const std::string& text);

/// Filesystem-safe compact form (no dashes) used inside managed storage names.
std::string uuidToCompact(const std::string& uuid);

}  // namespace trace
