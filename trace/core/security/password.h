#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/common/result.h"

namespace trace {

/// Password storage for local accounts.
///
/// SHA-256 on its own is the wrong tool here and is deliberately not used. It is
/// fast, which is exactly the property an attacker wants: a modern GPU tries
/// billions of plain SHA-256 guesses a second against a stolen database. A
/// password store has to be *slow* on purpose.
///
/// TRACE uses PBKDF2-HMAC-SHA256, which is deliberately conservative: it is
/// specified in RFC 8018, approved in NIST SP 800-132, and builds on the SHA-256
/// already in this codebase, so authentication adds no third-party cryptographic
/// dependency to software that has to be auditable. Argon2id resists custom
/// hardware better and would be the choice if a dependency were acceptable; the
/// stored format below names its algorithm precisely so that moving to it later
/// is a migration rather than a rewrite.
///
/// Nothing in this header ever accepts or returns a password in a form that is
/// written to disk or to a log. The plaintext exists only as an argument.
namespace password {

/// The work factor. OWASP's guidance for PBKDF2-HMAC-SHA256 is at least 600,000
/// iterations, which costs a fraction of a second once at login and multiplies
/// an offline attacker's cost by the same factor.
inline constexpr std::uint32_t kDefaultIterations = 600'000;

/// 128 bits, from the operating system's cryptographic source. The salt is not
/// a secret; its job is to make one precomputed table useless against every
/// account at once, which needs uniqueness rather than secrecy.
inline constexpr std::size_t kSaltBytes = 16;

inline constexpr std::size_t kKeyBytes = 32;

/// Names the algorithm in the stored record. Verification refuses anything it
/// does not recognise rather than guessing, so a row written by a future version
/// fails closed.
inline constexpr const char* kAlgorithmPbkdf2Sha256 = "pbkdf2-sha256";

/// What is persisted for one account. The plaintext is not among these.
struct StoredPassword {
    std::string algorithm = kAlgorithmPbkdf2Sha256;
    std::uint32_t iterations = kDefaultIterations;
    std::string saltHex;
    std::string hashHex;
};

/// HMAC-SHA256 (RFC 2104). Exposed because PBKDF2 is defined in terms of it and
/// because it is worth testing directly against the published vectors.
std::vector<std::uint8_t> hmacSha256(const std::vector<std::uint8_t>& key,
                                     const std::vector<std::uint8_t>& message);

/// PBKDF2-HMAC-SHA256 (RFC 8018 §5.2).
std::vector<std::uint8_t> pbkdf2Sha256(const std::string& password,
                                       const std::vector<std::uint8_t>& salt,
                                       std::uint32_t iterations, std::size_t keyLength);

/// Cryptographically secure random bytes from the operating system. Fails rather
/// than falling back to a predictable source: a salt or key from a weak
/// generator is worse than no security at all, because it looks like security.
Result<std::vector<std::uint8_t>> randomBytes(std::size_t count);

/// Derives a stored record for a new or changed password.
Result<StoredPassword> hash(const std::string& plaintext,
                            std::uint32_t iterations = kDefaultIterations);

/// True when `plaintext` matches `stored`.
///
/// The comparison is constant-time with respect to the digest contents. A
/// byte-by-byte compare that returns early leaks, through timing, how much of a
/// guess was right, which is enough to reconstruct a hash one byte at a time.
bool verify(const std::string& plaintext, const StoredPassword& stored);

/// Constant-time equality for two equal-length byte ranges.
bool constantTimeEquals(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b);

/// Whether a stored record should be re-derived on the next successful login —
/// because the work factor has since been raised, or the algorithm superseded.
bool needsRehash(const StoredPassword& stored,
                 std::uint32_t currentIterations = kDefaultIterations);

/// The rules a new password must satisfy, and why. Returned as a sentence fit to
/// show an operator rather than a code.
///
/// Length is the requirement that actually matters, so it is the one enforced.
/// Composition rules ("one capital, one symbol") push people towards
/// `Password1!` and are not imposed here.
Status checkStrength(const std::string& plaintext);

/// Minimum accepted length. NIST SP 800-63B sets 8 as the floor for
/// user-chosen secrets; TRACE requires 12 because a single shared workstation
/// account guards a whole case load.
inline constexpr std::size_t kMinimumLength = 12;

}  // namespace password
}  // namespace trace
