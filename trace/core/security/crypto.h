#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/common/result.h"

namespace trace {

/// Authenticated encryption for TRACE's data at rest.
///
/// ## Why this file depends on OpenSSL when password.h deliberately does not
///
/// `core/security/password.cpp` implements PBKDF2-HMAC-SHA256 on TRACE's own
/// SHA-256, and says in its header that authentication should add no third-party
/// cryptographic dependency to software that has to be auditable. That reasoning
/// does not carry over here, and the difference is worth stating rather than
/// quietly reversing.
///
/// PBKDF2 is a few dozen lines of iteration over a hash the codebase already
/// had, it is specified in RFC 8018, and it has published test vectors: a wrong
/// implementation fails those vectors loudly. AES-GCM is not like that. It has
/// failure modes that produce correct-looking output and destroy the security
/// anyway — reusing a nonce under one key reveals the XOR of two plaintexts and
/// leaks the authentication subkey outright — and a table-driven AES written
/// here would leak key material through cache timing while passing every test
/// vector. Correct ciphertext is not evidence of a correct implementation.
///
/// So: the primitive comes from libcrypto, which is audited, constant-time on
/// every platform TRACE targets, and hardware-accelerated. What this file adds
/// is the part that is TRACE's responsibility — key hierarchy, nonce discipline,
/// and a container format that cannot be silently truncated or reordered.
namespace crypto {

/// AES-256. Also the size of every key in the hierarchy, deliberately: one
/// length means no code path has to decide which it was handed.
inline constexpr std::size_t kKeyBytes = 32;
/// 96 bits, the nonce size AES-GCM is specified for and the only one that needs
/// no extra hashing step.
inline constexpr std::size_t kNonceBytes = 12;
/// The full 128-bit GCM tag. Truncated tags are permitted by the standard and
/// are not used here; nothing in TRACE is short of 16 bytes.
inline constexpr std::size_t kTagBytes = 16;
/// Per-file salt for the subkey derivation described under `EncryptedFileWriter`.
inline constexpr std::size_t kFileSaltBytes = 16;

/// True when this build can encrypt and decrypt. False builds still open
/// unencrypted workspaces; they refuse encrypted ones with a stated reason
/// rather than failing somewhere further down.
bool available();

/// A 256-bit symmetric key.
///
/// The bytes are wiped when the key is destroyed. That is a smaller guarantee
/// than it sounds — a key that has been swapped out or copied by a reallocating
/// container has already escaped — so it is a floor, not a claim. It costs
/// nothing and removes the most common way a key outlives its use: a freed
/// buffer handed straight back to the next allocation.
class SecretKey {
public:
    SecretKey();
    ~SecretKey();
    SecretKey(const SecretKey& other);
    SecretKey& operator=(const SecretKey& other);
    SecretKey(SecretKey&& other) noexcept;
    SecretKey& operator=(SecretKey&& other) noexcept;

    /// From the operating system's cryptographic source. Fails rather than
    /// falling back; see password::randomBytes for why.
    static Result<SecretKey> random();

    /// Adopts exactly kKeyBytes of existing key material.
    static Result<SecretKey> fromBytes(const std::vector<std::uint8_t>& bytes);

    /// Derives a key from a password with PBKDF2-HMAC-SHA256, reusing the work
    /// factor and implementation that local accounts already use.
    static Result<SecretKey> fromPassword(const std::string& password,
                                          const std::vector<std::uint8_t>& salt,
                                          std::uint32_t iterations);

    /// HKDF-SHA256 (RFC 5869) on this key, for subkeys that must be independent
    /// of it and of each other. Built on password::hmacSha256 — no new
    /// primitive, because HKDF genuinely is just HMAC in a documented order.
    Result<SecretKey> deriveSubkey(const std::vector<std::uint8_t>& salt,
                                   const std::string& info) const;

    const std::uint8_t* data() const { return bytes_.data(); }
    std::size_t size() const { return bytes_.size(); }

    /// Lowercase hex, for the one caller that needs it: SQLCipher takes a raw
    /// key as a hex literal. Handling it as text is unavoidable there; nothing
    /// else should use this.
    std::string toHexForSqlCipher() const;

private:
    std::vector<std::uint8_t> bytes_;
};

/// Ciphertext of a small value, with everything needed to open it again.
///
/// Layout on disk and in the database is nonce ‖ ciphertext ‖ tag, so a sealed
/// blob is one opaque field rather than three columns that could be mismatched.
Result<std::vector<std::uint8_t>> seal(const SecretKey& key,
                                       const std::vector<std::uint8_t>& plaintext,
                                       const std::string& associatedData);

/// Inverse of `seal`. Fails when the key is wrong, the associated data differs,
/// or a single bit has changed — all three are the same answer, deliberately:
/// distinguishing them tells an attacker which part of a forgery was accepted.
Result<std::vector<std::uint8_t>> unseal(const SecretKey& key,
                                         const std::vector<std::uint8_t>& sealed,
                                         const std::string& associatedData);

/// Bytes of a TRACE encrypted container header, for callers that need to know
/// whether a file is one without opening it.
inline constexpr std::size_t kContainerHeaderBytes = 48;
/// "TRACEEV1" — recognisable in a hex dump, and enough to refuse a file that is
/// not a container rather than decrypting noise.
extern const char kContainerMagic[9];

/// Default plaintext chunk. Large enough that per-chunk overhead is under a
/// thousandth, small enough that seeking to a frame does not decrypt megabytes.
inline constexpr std::size_t kDefaultChunkBytes = 256 * 1024;

/// Writes a TRACE encrypted container.
///
/// ### Format
///
///     magic        8   "TRACEEV1"
///     version      2   little-endian, currently 1
///     algorithm    2   1 = AES-256-GCM
///     chunkBytes   4   plaintext bytes per chunk
///     plainSize    8   total plaintext length
///     salt        16   per-file, for the subkey derivation below
///     reserved     8   zero
///     ---------------  48 bytes, then one record per chunk:
///     ciphertext  n    n = chunkBytes, except the last
///     tag         16
///
/// ### Why a per-file subkey
///
/// Every chunk under one key needs its own nonce, and reusing one is fatal. The
/// usual construction is a random prefix plus a counter, which is sound but
/// leaves a birthday bound to reason about across a whole case. Instead each
/// file derives its own key — HKDF over the case key with a random per-file salt
/// — and then the nonce is simply the chunk index. A key used for exactly one
/// file cannot repeat a nonce, so the property holds by construction rather than
/// by argument about how many files a case will ever hold.
///
/// ### Why truncation is detected
///
/// Each chunk's associated data is the full header plus that chunk's index, so a
/// chunk cannot be moved, duplicated, or lifted from another file. The header
/// carries the plaintext length, so a container cut short fails on the missing
/// chunk rather than reading as a shorter recording — which for evidence is the
/// difference between a detected fault and a silently altered exhibit.
class EncryptedFileWriter {
public:
    EncryptedFileWriter();
    ~EncryptedFileWriter();
    EncryptedFileWriter(const EncryptedFileWriter&) = delete;
    EncryptedFileWriter& operator=(const EncryptedFileWriter&) = delete;

    /// `plaintextSize` must be known up front: it goes in the header, which is
    /// authenticated, so it cannot be patched in afterwards.
    Status begin(const std::filesystem::path& path, const SecretKey& caseKey,
                 std::uint64_t plaintextSize, std::size_t chunkBytes = kDefaultChunkBytes);
    Status write(const std::uint8_t* data, std::size_t bytes);
    /// Fails when fewer bytes were written than the header promised.
    Status finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Random-access reader for a container written by `EncryptedFileWriter`.
///
/// `read` at an arbitrary offset decrypts only the chunks it spans, which is
/// what lets FFmpeg seek through an encrypted recording without decrypting the
/// whole of it. One decrypted chunk is cached, because sequential decoding reads
/// far smaller pieces than a chunk at a time.
class EncryptedFileReader {
public:
    EncryptedFileReader();
    ~EncryptedFileReader();
    EncryptedFileReader(const EncryptedFileReader&) = delete;
    EncryptedFileReader& operator=(const EncryptedFileReader&) = delete;

    Status open(const std::filesystem::path& path, const SecretKey& caseKey);

    /// Plaintext length, from the authenticated header.
    std::uint64_t size() const;

    /// Reads up to `bytes` from `offset`. A short read means end of file; a
    /// failure means the container did not authenticate, which is never
    /// something to retry or work around.
    Result<std::size_t> read(std::uint64_t offset, std::uint8_t* into, std::size_t bytes);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// True when the file begins with the container magic. Cheap, and not a
/// security check: it says what a file claims to be, not that it is intact.
bool looksEncrypted(const std::filesystem::path& path);

}  // namespace crypto
}  // namespace trace
