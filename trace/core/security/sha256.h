#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace trace {

/// FIPS 180-4 SHA-256, streaming.
///
/// TRACE hashes originals in bounded chunks rather than reading whole files
/// into memory, because evidence routinely runs to many gigabytes. The
/// implementation is self-contained (no OpenSSL dependency) and is validated in
/// the unit suite against the published NIST test vectors.
class Sha256 {
public:
    static constexpr std::size_t kDigestSize = 32;
    using Digest = std::array<std::uint8_t, kDigestSize>;

    Sha256();

    void reset();
    void update(const void* data, std::size_t size);
    void update(std::string_view text);

    /// Finalises the hash. The object must be reset() before reuse.
    Digest finalizeBytes();
    std::string finalizeHex();

    static std::string toHex(const Digest& digest);

    /// One-shot helpers.
    static std::string hash(std::string_view text);

private:
    void compress(const std::uint8_t block[64]);

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t bufferSize_ = 0;
    std::uint64_t totalBits_ = 0;
    bool finalized_ = false;
};

}  // namespace trace
