// Password hashing and verification.
//
// The HMAC and PBKDF2 cases below are known-answer tests against the published
// vectors (RFC 4231 for HMAC-SHA256, and the widely republished SHA-256
// counterparts of the RFC 6070 PBKDF2 inputs). A hand-rolled KDF that has not
// been checked against somebody else's numbers is a guess.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/security/password.h"

namespace trace {
namespace {

std::string hex(const std::vector<std::uint8_t>& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (std::uint8_t b : bytes) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0F]);
    }
    return out;
}

std::vector<std::uint8_t> bytes(const std::string& text) { return {text.begin(), text.end()}; }

TEST(PasswordTest, HmacMatchesTheRfc4231Vectors) {
    EXPECT_EQ(hex(password::hmacSha256(std::vector<std::uint8_t>(20, 0x0b), bytes("Hi There"))),
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    EXPECT_EQ(hex(password::hmacSha256(bytes("Jefe"), bytes("what do ya want for nothing?"))),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    EXPECT_EQ(hex(password::hmacSha256(std::vector<std::uint8_t>(20, 0xaa),
                                       std::vector<std::uint8_t>(50, 0xdd))),
              "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");

    // Longer than the 64-byte block, which exercises the hash-the-key branch.
    EXPECT_EQ(hex(password::hmacSha256(
                  std::vector<std::uint8_t>(131, 0xaa),
                  bytes("Test Using Larger Than Block-Size Key - Hash Key First"))),
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST(PasswordTest, Pbkdf2MatchesThePublishedVectors) {
    EXPECT_EQ(hex(password::pbkdf2Sha256("password", bytes("salt"), 1, 32)),
              "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    EXPECT_EQ(hex(password::pbkdf2Sha256("password", bytes("salt"), 2, 32)),
              "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
    EXPECT_EQ(hex(password::pbkdf2Sha256("password", bytes("salt"), 4096, 32)),
              "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");

    // A key longer than one SHA-256 block, which is the only case that runs the
    // block-concatenation loop more than once.
    EXPECT_EQ(hex(password::pbkdf2Sha256("passwordPASSWORDpassword",
                                         bytes("saltSALTsaltSALTsaltSALTsaltSALTsalt"), 4096, 40)),
              "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c635518c7dac47e9");
}

TEST(PasswordTest, TheWorkFactorIsHighEnoughToBeWorthHaving) {
    // The whole point of a KDF is that it is expensive. OWASP's floor for
    // PBKDF2-HMAC-SHA256 is 600,000; a future edit that quietly lowered it to
    // something fast would defeat the entire mechanism, so the constant is
    // asserted rather than trusted.
    EXPECT_GE(password::kDefaultIterations, 600'000u);
    EXPECT_GE(password::kSaltBytes, 16u);
    EXPECT_GE(password::kKeyBytes, 32u);
}

TEST(PasswordTest, VerifiesTheRightPasswordAndRejectsEverythingElse) {
    auto stored = password::hash("correct horse battery staple");
    ASSERT_TRUE(stored.ok()) << stored.error().message();

    EXPECT_TRUE(password::verify("correct horse battery staple", stored.value()));
    EXPECT_FALSE(password::verify("correct horse battery stapl", stored.value()));
    EXPECT_FALSE(password::verify("Correct horse battery staple", stored.value()));
    EXPECT_FALSE(password::verify("", stored.value()));
}

TEST(PasswordTest, TheSamePasswordHashesDifferentlyEveryTime) {
    auto first = password::hash("correct horse battery staple");
    auto second = password::hash("correct horse battery staple");
    ASSERT_TRUE(first.ok());
    ASSERT_TRUE(second.ok());

    // Different salts, so two accounts sharing a password are not visibly
    // sharing one, and one precomputed table cannot cover both.
    EXPECT_NE(first.value().saltHex, second.value().saltHex);
    EXPECT_NE(first.value().hashHex, second.value().hashHex);
    EXPECT_TRUE(password::verify("correct horse battery staple", first.value()));
    EXPECT_TRUE(password::verify("correct horse battery staple", second.value()));
}

TEST(PasswordTest, TheStoredRecordNeverContainsThePassword) {
    const std::string secret = "correct horse battery staple";
    auto stored = password::hash(secret);
    ASSERT_TRUE(stored.ok());

    EXPECT_EQ(stored.value().hashHex.find(secret), std::string::npos);
    EXPECT_EQ(stored.value().saltHex.find(secret), std::string::npos);
    EXPECT_EQ(stored.value().algorithm.find(secret), std::string::npos);
}

TEST(PasswordTest, AnUnrecognisedOrCorruptRecordFailsClosed) {
    auto stored = password::hash("correct horse battery staple");
    ASSERT_TRUE(stored.ok());

    // An algorithm this build does not know — a row written by a later version.
    auto future = stored.value();
    future.algorithm = "argon2id";
    EXPECT_FALSE(password::verify("correct horse battery staple", future))
        << "an unknown algorithm must never be verified under an assumed one";

    // A work factor of zero would otherwise make verification trivially cheap.
    auto zero = stored.value();
    zero.iterations = 0;
    EXPECT_FALSE(password::verify("correct horse battery staple", zero));

    // Hex that does not parse must not be read as a string of zero bytes.
    auto corrupt = stored.value();
    corrupt.hashHex = "not hex at all";
    EXPECT_FALSE(password::verify("correct horse battery staple", corrupt));

    auto badSalt = stored.value();
    badSalt.saltHex = "zzzz";
    EXPECT_FALSE(password::verify("correct horse battery staple", badSalt));
}

TEST(PasswordTest, ARaisedWorkFactorMarksOlderRecordsForRehashing) {
    auto stored = password::hash("correct horse battery staple", 600'000);
    ASSERT_TRUE(stored.ok());

    EXPECT_FALSE(password::needsRehash(stored.value(), 600'000));
    EXPECT_TRUE(password::needsRehash(stored.value(), 1'200'000))
        << "an account hashed under the old factor has to be identifiable to be upgraded";

    auto foreign = stored.value();
    foreign.algorithm = "argon2id";
    EXPECT_TRUE(password::needsRehash(foreign, 600'000));
}

TEST(PasswordTest, ConstantTimeComparisonStillCompares) {
    // Timing cannot be asserted portably; correctness can, and a comparison that
    // is constant-time but wrong would be worse than the naive one.
    EXPECT_TRUE(password::constantTimeEquals({1, 2, 3}, {1, 2, 3}));
    EXPECT_FALSE(password::constantTimeEquals({1, 2, 3}, {1, 2, 4}));
    EXPECT_FALSE(password::constantTimeEquals({1, 2, 3}, {1, 2}));
    EXPECT_TRUE(password::constantTimeEquals({}, {}));
}

TEST(PasswordTest, LengthIsRequiredAndCompositionIsNot) {
    EXPECT_FALSE(password::checkStrength("short"));
    EXPECT_FALSE(password::checkStrength("elevenchar"));
    EXPECT_TRUE(password::checkStrength("twelvechars!"));

    // A long passphrase of nothing but lowercase letters is accepted, because
    // length is what resists guessing. Demanding a capital and a symbol produces
    // Password1! and narrows the space an attacker searches.
    EXPECT_TRUE(password::checkStrength("correct horse battery staple"));

    // And hashing enforces the same rule rather than leaving it to callers.
    EXPECT_FALSE(password::hash("short").ok());
}

TEST(PasswordTest, TheRandomSourceProducesDistinctBytes) {
    auto first = password::randomBytes(32);
    auto second = password::randomBytes(32);
    ASSERT_TRUE(first.ok()) << first.error().message();
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(first.value().size(), 32u);
    EXPECT_NE(first.value(), second.value());

    // Not a randomness test — it cannot be, from two samples. It catches the
    // failure that matters: a source that returns a fixed buffer or nothing.
    bool allZero = true;
    for (auto byte : first.value()) {
        if (byte != 0) allZero = false;
    }
    EXPECT_FALSE(allZero);
}

}  // namespace
}  // namespace trace
