// Tests for TRACE's encryption primitives.
//
// These do not try to re-test AES-GCM: libcrypto's implementation is audited and
// has NIST's own vectors run against it. What is TRACE's responsibility, and so
// what is tested here, is everything wrapped around it — that a key derivation
// follows the specification rather than something that merely looks like it,
// that nonces cannot repeat, that a container cannot be truncated, reordered, or
// read with the wrong key without the failure being loud.
//
// Every "this must fail" case below is a case where succeeding would mean TRACE
// had accepted altered evidence.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <random>
#include <vector>

#include "core/security/crypto.h"
#include "core/security/password.h"
#include "tests/support/test_environment.h"

namespace trace {
namespace {

std::vector<std::uint8_t> fromHex(const std::string& hex) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        bytes.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

std::string toHex(const std::uint8_t* data, std::size_t bytes) {
    static const char* digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(bytes * 2);
    for (std::size_t i = 0; i < bytes; ++i) {
        hex.push_back(digits[data[i] >> 4]);
        hex.push_back(digits[data[i] & 0x0F]);
    }
    return hex;
}

crypto::SecretKey keyOfBytes(std::uint8_t seed) {
    std::vector<std::uint8_t> bytes(crypto::kKeyBytes);
    std::iota(bytes.begin(), bytes.end(), seed);
    auto key = crypto::SecretKey::fromBytes(bytes);
    EXPECT_TRUE(key.ok());
    return key.take();
}

/// A scratch file in a directory that removes itself, so a failing assertion
/// does not leave a container behind for the next run to trip over. Uses the
/// suite's own temporary directory rather than a hand-built name, which is also
/// what keeps this test portable off POSIX.
class ScratchFile {
public:
    explicit ScratchFile(const std::string& name)
        : directory_("trace-crypto"), path_(directory_ / name) {}
    const std::filesystem::path& path() const { return path_; }

private:
    testing::TemporaryDirectory directory_;
    std::filesystem::path path_;
};

std::vector<std::uint8_t> pattern(std::size_t bytes) {
    // Not random: a repeatable pattern makes a wrong offset obvious in a
    // failure message instead of showing two piles of noise.
    std::vector<std::uint8_t> data(bytes);
    for (std::size_t i = 0; i < bytes; ++i) {
        data[i] = static_cast<std::uint8_t>((i * 31 + (i >> 8) * 7) & 0xFF);
    }
    return data;
}

/// Everything below the key-derivation block needs the AEAD primitive, which a
/// build without OpenSSL does not have. Key derivation and the random source are
/// TRACE's own code and work in any build, so those stay plain TEST cases.
class CryptoSealing : public ::testing::Test {
protected:
    void SetUp() override {
        if (!crypto::available()) GTEST_SKIP() << "built without encryption support";
    }
};

class ContainerRoundTrip : public ::testing::TestWithParam<std::size_t> {
protected:
    void SetUp() override {
        if (!crypto::available()) GTEST_SKIP() << "built without encryption support";
    }
};

// ------------------------------------------------------------ key derivation

TEST(Crypto, SubkeyDerivationMatchesAnIndependentHkdfImplementation) {
    // HKDF-SHA256 (RFC 5869) computed with Python's hmac/hashlib, which shares
    // no code with TRACE. This is the test that catches the classic error of
    // passing the salt as the message and the key material as the HMAC key:
    // that mistake still produces 32 plausible bytes, and only a vector from
    // somewhere else notices.
    const auto ikm = fromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    const auto salt = fromHex("a0a1a2a3a4a5a6a7a8a9aaabacadaeaf");

    auto key = crypto::SecretKey::fromBytes(ikm);
    ASSERT_TRUE(key.ok());
    auto derived = key.take().deriveSubkey(salt, "trace-evidence-v1");
    ASSERT_TRUE(derived.ok());
    const auto subkey = derived.take();

    EXPECT_EQ(toHex(subkey.data(), subkey.size()),
              "be6daede205b7ac201b7bfd83ca7b2aa14fd1f8d506086702f31a6ae2c3860b9");
}

TEST(Crypto, SubkeysDifferWhenTheSaltOrTheInfoDiffers) {
    const auto key = keyOfBytes(1);
    const auto saltA = fromHex("00112233445566778899aabbccddeeff");
    const auto saltB = fromHex("00112233445566778899aabbccddee00");

    auto a = key.deriveSubkey(saltA, "trace-evidence-v1");
    auto b = key.deriveSubkey(saltB, "trace-evidence-v1");
    auto c = key.deriveSubkey(saltA, "trace-something-else");
    ASSERT_TRUE(a.ok() && b.ok() && c.ok());

    const auto keyA = a.take();
    const auto keyB = b.take();
    const auto keyC = c.take();
    EXPECT_NE(toHex(keyA.data(), keyA.size()), toHex(keyB.data(), keyB.size()));
    EXPECT_NE(toHex(keyA.data(), keyA.size()), toHex(keyC.data(), keyC.size()));
}

TEST(Crypto, ARandomKeyIsNotTheSameTwice) {
    auto first = crypto::SecretKey::random();
    auto second = crypto::SecretKey::random();
    ASSERT_TRUE(first.ok() && second.ok());
    const auto a = first.take();
    const auto b = second.take();
    EXPECT_NE(toHex(a.data(), a.size()), toHex(b.data(), b.size()));
    // An all-zero key would mean the random source silently produced nothing.
    EXPECT_NE(toHex(a.data(), a.size()), std::string(crypto::kKeyBytes * 2, '0'));
}

TEST(Crypto, AKeyMustBeExactlyThirtyTwoBytes) {
    EXPECT_FALSE(crypto::SecretKey::fromBytes(std::vector<std::uint8_t>(16, 0)).ok());
    EXPECT_FALSE(crypto::SecretKey::fromBytes(std::vector<std::uint8_t>(33, 0)).ok());
    EXPECT_TRUE(crypto::SecretKey::fromBytes(std::vector<std::uint8_t>(32, 0)).ok());
}

// ----------------------------------------------------------- sealed values

TEST_F(CryptoSealing, SealAndUnsealRoundTrip) {
    const auto key = keyOfBytes(7);
    const std::vector<std::uint8_t> plaintext = {'c', 'a', 's', 'e', ' ', 'k', 'e', 'y'};

    auto sealed = crypto::seal(key, plaintext, "workspace");
    ASSERT_TRUE(sealed.ok()) << sealed.error().toString();
    auto opened = crypto::unseal(key, sealed.take(), "workspace");
    ASSERT_TRUE(opened.ok()) << opened.error().toString();
    EXPECT_EQ(opened.take(), plaintext);
}

TEST_F(CryptoSealing, SealingTheSameValueTwiceProducesDifferentCiphertext) {
    // If it did not, the nonce would be fixed, and two values sealed under one
    // key would leak their relationship. This is the cheapest test that catches
    // the single most damaging mistake available in GCM.
    const auto key = keyOfBytes(9);
    const std::vector<std::uint8_t> plaintext(64, 0xAB);

    auto first = crypto::seal(key, plaintext, "");
    auto second = crypto::seal(key, plaintext, "");
    ASSERT_TRUE(first.ok() && second.ok());
    EXPECT_NE(first.take(), second.take());
}

TEST_F(CryptoSealing, UnsealRefusesTheWrongKey) {
    const std::vector<std::uint8_t> plaintext = {1, 2, 3, 4};
    auto sealed = crypto::seal(keyOfBytes(1), plaintext, "aad");
    ASSERT_TRUE(sealed.ok());

    auto opened = crypto::unseal(keyOfBytes(2), sealed.take(), "aad");
    ASSERT_FALSE(opened.ok());
    EXPECT_EQ(opened.error().code(), ErrorCode::IntegrityFailure);
}

TEST_F(CryptoSealing, UnsealRefusesDifferentAssociatedData) {
    // The associated data binds a sealed key to the case it belongs to. Lifting
    // one case's wrapped key into another case's row has to fail, and this is
    // what makes it fail.
    const auto key = keyOfBytes(3);
    auto sealed = crypto::seal(key, {9, 9, 9}, "case:AAA");
    ASSERT_TRUE(sealed.ok());

    auto opened = crypto::unseal(key, sealed.take(), "case:BBB");
    ASSERT_FALSE(opened.ok());
    EXPECT_EQ(opened.error().code(), ErrorCode::IntegrityFailure);
}

TEST_F(CryptoSealing, UnsealRefusesASingleFlippedBit) {
    const auto key = keyOfBytes(5);
    auto sealed = crypto::seal(key, pattern(200), "aad");
    ASSERT_TRUE(sealed.ok());
    auto bytes = sealed.take();

    for (std::size_t position : {std::size_t{0}, bytes.size() / 2, bytes.size() - 1}) {
        auto damaged = bytes;
        damaged[position] ^= 0x01;
        auto opened = crypto::unseal(key, damaged, "aad");
        EXPECT_FALSE(opened.ok()) << "a flipped bit at " << position << " was accepted";
    }
}

TEST_F(CryptoSealing, UnsealRefusesAValueTooShortToBeIntact) {
    auto opened = crypto::unseal(keyOfBytes(1), std::vector<std::uint8_t>(4, 0), "");
    ASSERT_FALSE(opened.ok());
    EXPECT_EQ(opened.error().code(), ErrorCode::IntegrityFailure);
}

// --------------------------------------------------------------- containers

TEST_P(ContainerRoundTrip, PlaintextSurvivesExactly) {
    const std::size_t bytes = GetParam();
    const auto key = keyOfBytes(11);
    const auto plaintext = pattern(bytes);

    ScratchFile file("roundtrip" + std::to_string(bytes));
    {
        crypto::EncryptedFileWriter writer;
        // A small chunk so the sizes below straddle chunk boundaries without
        // needing megabyte test files.
        ASSERT_TRUE(writer.begin(file.path(), key, bytes, 1024).ok());
        ASSERT_TRUE(writer.write(plaintext.data(), plaintext.size()).ok());
        ASSERT_TRUE(writer.finish().ok());
    }

    crypto::EncryptedFileReader reader;
    ASSERT_TRUE(reader.open(file.path(), key).ok());
    EXPECT_EQ(reader.size(), bytes);

    std::vector<std::uint8_t> read(bytes);
    auto got = reader.read(0, read.data(), read.size());
    ASSERT_TRUE(got.ok()) << got.error().toString();
    EXPECT_EQ(got.take(), bytes);
    EXPECT_EQ(read, plaintext);
}

// Empty, under one chunk, exactly one chunk, one byte over, several chunks, and
// a size that is not a multiple of the chunk — the boundaries where an
// off-by-one in the last-chunk length would hide.
INSTANTIATE_TEST_SUITE_P(Sizes, ContainerRoundTrip,
                         ::testing::Values(0u, 1u, 1023u, 1024u, 1025u, 4096u, 5000u));

TEST_F(CryptoSealing, ContainerIsNotThePlaintext) {
    // The point of the exercise: what lands on disk must not contain the bytes
    // that went in.
    const auto key = keyOfBytes(13);
    const std::string secret = "SUSPECT INTERVIEW 2026-03-11";
    const std::vector<std::uint8_t> plaintext(secret.begin(), secret.end());

    ScratchFile file("opaque");
    crypto::EncryptedFileWriter writer;
    ASSERT_TRUE(writer.begin(file.path(), key, plaintext.size()).ok());
    ASSERT_TRUE(writer.write(plaintext.data(), plaintext.size()).ok());
    ASSERT_TRUE(writer.finish().ok());

    std::ifstream in(file.path(), std::ios::binary);
    const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(raw.find(secret), std::string::npos);
    EXPECT_TRUE(crypto::looksEncrypted(file.path()));
}

TEST_F(CryptoSealing, RandomAccessReadsMatchTheSameOffsetsInThePlaintext) {
    // This is what lets FFmpeg seek. If it were wrong, a recording would decode
    // to the wrong frames rather than fail, which is the worst possible failure
    // mode for evidence.
    const std::size_t bytes = 10000;
    const auto key = keyOfBytes(17);
    const auto plaintext = pattern(bytes);

    ScratchFile file("random");
    {
        crypto::EncryptedFileWriter writer;
        ASSERT_TRUE(writer.begin(file.path(), key, bytes, 1024).ok());
        ASSERT_TRUE(writer.write(plaintext.data(), plaintext.size()).ok());
        ASSERT_TRUE(writer.finish().ok());
    }

    crypto::EncryptedFileReader reader;
    ASSERT_TRUE(reader.open(file.path(), key).ok());

    std::mt19937 rng(20260830);
    std::uniform_int_distribution<std::size_t> offsets(0, bytes - 1);
    std::uniform_int_distribution<std::size_t> lengths(1, 3000);
    for (int attempt = 0; attempt < 200; ++attempt) {
        const std::size_t offset = offsets(rng);
        const std::size_t wanted = std::min(lengths(rng), bytes - offset);
        std::vector<std::uint8_t> read(wanted);
        auto got = reader.read(offset, read.data(), wanted);
        ASSERT_TRUE(got.ok()) << got.error().toString();
        ASSERT_EQ(got.take(), wanted);
        ASSERT_TRUE(std::equal(read.begin(), read.end(), plaintext.begin() + offset))
            << "offset " << offset << " length " << wanted;
    }
}

TEST_F(CryptoSealing, ReadingPastTheEndReturnsNothingRatherThanFailing) {
    const auto key = keyOfBytes(19);
    const auto plaintext = pattern(100);
    ScratchFile file("past-end");
    {
        crypto::EncryptedFileWriter writer;
        ASSERT_TRUE(writer.begin(file.path(), key, plaintext.size()).ok());
        ASSERT_TRUE(writer.write(plaintext.data(), plaintext.size()).ok());
        ASSERT_TRUE(writer.finish().ok());
    }
    crypto::EncryptedFileReader reader;
    ASSERT_TRUE(reader.open(file.path(), key).ok());

    std::vector<std::uint8_t> read(50);
    auto got = reader.read(100, read.data(), read.size());
    ASSERT_TRUE(got.ok());
    EXPECT_EQ(got.take(), 0u);

    // A read that starts inside and runs past the end is short, not an error.
    auto partial = reader.read(90, read.data(), read.size());
    ASSERT_TRUE(partial.ok());
    EXPECT_EQ(partial.take(), 10u);
}

TEST_F(CryptoSealing, AContainerCannotBeOpenedWithADifferentCaseKey) {
    const auto plaintext = pattern(2000);
    ScratchFile file("wrong-key");
    {
        crypto::EncryptedFileWriter writer;
        ASSERT_TRUE(writer.begin(file.path(), keyOfBytes(21), plaintext.size(), 512).ok());
        ASSERT_TRUE(writer.write(plaintext.data(), plaintext.size()).ok());
        ASSERT_TRUE(writer.finish().ok());
    }

    crypto::EncryptedFileReader reader;
    // Opening succeeds — the header is not secret — but nothing decrypts.
    ASSERT_TRUE(reader.open(file.path(), keyOfBytes(22)).ok());
    std::vector<std::uint8_t> read(100);
    auto got = reader.read(0, read.data(), read.size());
    ASSERT_FALSE(got.ok());
    EXPECT_EQ(got.error().code(), ErrorCode::IntegrityFailure);
}

TEST_F(CryptoSealing, ATruncatedContainerFailsRatherThanReadingShort) {
    // A recording cut short must not present itself as a shorter recording.
    const auto key = keyOfBytes(23);
    const auto plaintext = pattern(4000);
    ScratchFile file("truncated");
    {
        crypto::EncryptedFileWriter writer;
        ASSERT_TRUE(writer.begin(file.path(), key, plaintext.size(), 1024).ok());
        ASSERT_TRUE(writer.write(plaintext.data(), plaintext.size()).ok());
        ASSERT_TRUE(writer.finish().ok());
    }

    const auto full = std::filesystem::file_size(file.path());
    std::filesystem::resize_file(file.path(), full - 600);

    crypto::EncryptedFileReader reader;
    ASSERT_TRUE(reader.open(file.path(), key).ok());
    // The header still claims 4000 bytes, and it is authenticated, so the size
    // cannot have been quietly reduced along with the data.
    EXPECT_EQ(reader.size(), 4000u);

    std::vector<std::uint8_t> read(4000);
    auto got = reader.read(0, read.data(), read.size());
    ASSERT_FALSE(got.ok());
    EXPECT_EQ(got.error().code(), ErrorCode::IntegrityFailure);
}

TEST_F(CryptoSealing, ReorderingChunksIsDetected) {
    // Each chunk is authenticated against its own index, so a container whose
    // chunks have been swapped is not a valid container at either position.
    const auto key = keyOfBytes(29);
    const std::size_t chunk = 1024;
    const auto plaintext = pattern(chunk * 3);
    ScratchFile file("reordered");
    {
        crypto::EncryptedFileWriter writer;
        ASSERT_TRUE(writer.begin(file.path(), key, plaintext.size(), chunk).ok());
        ASSERT_TRUE(writer.write(plaintext.data(), plaintext.size()).ok());
        ASSERT_TRUE(writer.finish().ok());
    }

    std::vector<char> raw;
    {
        std::ifstream in(file.path(), std::ios::binary);
        raw.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    const std::size_t record = chunk + crypto::kTagBytes;
    const std::size_t first = crypto::kContainerHeaderBytes;
    const std::size_t second = first + record;
    std::swap_ranges(raw.begin() + static_cast<long>(first),
                     raw.begin() + static_cast<long>(first + record),
                     raw.begin() + static_cast<long>(second));
    {
        std::ofstream out(file.path(), std::ios::binary | std::ios::trunc);
        out.write(raw.data(), static_cast<std::streamsize>(raw.size()));
    }

    crypto::EncryptedFileReader reader;
    ASSERT_TRUE(reader.open(file.path(), key).ok());
    std::vector<std::uint8_t> read(chunk);
    auto got = reader.read(0, read.data(), read.size());
    ASSERT_FALSE(got.ok());
    EXPECT_EQ(got.error().code(), ErrorCode::IntegrityFailure);
}

TEST_F(CryptoSealing, EditingTheHeaderInvalidatesTheWholeFile) {
    // The header is every chunk's associated data, so changing the declared
    // length — the obvious way to hide a truncation — breaks decryption rather
    // than producing a plausible shorter file.
    const auto key = keyOfBytes(31);
    const auto plaintext = pattern(3000);
    ScratchFile file("header");
    {
        crypto::EncryptedFileWriter writer;
        ASSERT_TRUE(writer.begin(file.path(), key, plaintext.size(), 1024).ok());
        ASSERT_TRUE(writer.write(plaintext.data(), plaintext.size()).ok());
        ASSERT_TRUE(writer.finish().ok());
    }

    {
        std::fstream out(file.path(), std::ios::binary | std::ios::in | std::ios::out);
        out.seekp(16);  // plainSize
        const char shorter[8] = {0x00, 0x04, 0, 0, 0, 0, 0, 0};  // 1024
        out.write(shorter, sizeof(shorter));
    }

    crypto::EncryptedFileReader reader;
    ASSERT_TRUE(reader.open(file.path(), key).ok());
    EXPECT_EQ(reader.size(), 1024u);
    std::vector<std::uint8_t> read(1024);
    auto got = reader.read(0, read.data(), read.size());
    ASSERT_FALSE(got.ok());
    EXPECT_EQ(got.error().code(), ErrorCode::IntegrityFailure);
}

TEST_F(CryptoSealing, WritingFewerBytesThanDeclaredIsAnError) {
    // The header promises a length before the data is written. A caller that
    // stops early has produced a file that cannot be fully read, and finish()
    // is the last moment anyone can be told.
    const auto key = keyOfBytes(37);
    ScratchFile file("short-write");
    crypto::EncryptedFileWriter writer;
    ASSERT_TRUE(writer.begin(file.path(), key, 5000).ok());
    const auto some = pattern(100);
    ASSERT_TRUE(writer.write(some.data(), some.size()).ok());
    auto finished = writer.finish();
    EXPECT_FALSE(finished.ok());
}

TEST_F(CryptoSealing, AFileThatIsNotAContainerIsRefused) {
    ScratchFile file("not-a-container");
    {
        std::ofstream out(file.path(), std::ios::binary);
        const std::string junk(200, 'x');
        out << junk;
    }
    EXPECT_FALSE(crypto::looksEncrypted(file.path()));

    crypto::EncryptedFileReader reader;
    auto opened = reader.open(file.path(), keyOfBytes(41));
    EXPECT_FALSE(opened.ok());
}

TEST_F(CryptoSealing, AnEmptyFileIsRefusedRatherThanReadAsAnEmptyContainer) {
    ScratchFile file("empty");
    { std::ofstream out(file.path(), std::ios::binary); }
    crypto::EncryptedFileReader reader;
    auto opened = reader.open(file.path(), keyOfBytes(43));
    EXPECT_FALSE(opened.ok());
}

TEST_F(CryptoSealing, TwoContainersOfTheSamePlaintextDifferOnDisk) {
    // Each file derives its own key from a fresh salt, so identical evidence
    // ingested into two cases does not produce identical ciphertext — which
    // would otherwise reveal that the two are the same recording.
    const auto key = keyOfBytes(47);
    const auto plaintext = pattern(2048);

    auto writeOne = [&](const std::filesystem::path& path) {
        crypto::EncryptedFileWriter writer;
        EXPECT_TRUE(writer.begin(path, key, plaintext.size()).ok());
        EXPECT_TRUE(writer.write(plaintext.data(), plaintext.size()).ok());
        EXPECT_TRUE(writer.finish().ok());
    };
    auto contents = [](const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };

    ScratchFile a("same-a");
    ScratchFile b("same-b");
    writeOne(a.path());
    writeOne(b.path());
    EXPECT_NE(contents(a.path()), contents(b.path()));
}

TEST(Crypto, ThisBuildReportsWhetherItCanEncrypt) {
    // Not an assertion about which answer is right — a build without SQLCipher
    // or OpenSSL is a supported configuration. What matters is that the answer
    // is the same one the rest of TRACE acts on.
    EXPECT_EQ(crypto::available(), crypto::seal(keyOfBytes(1), {1, 2, 3}, "").ok());
}

}  // namespace
}  // namespace trace
