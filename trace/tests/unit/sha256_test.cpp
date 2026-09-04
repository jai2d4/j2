// SHA-256 must be exactly right: it is the whole basis of TRACE's integrity
// claim. These vectors are the published NIST/RFC values.
#include <gtest/gtest.h>

#include <fstream>
#include <string>

#include "core/security/file_hasher.h"
#include "core/security/sha256.h"
#include "tests/support/test_environment.h"

namespace trace {
namespace {

TEST(Sha256, EmptyInputMatchesPublishedVector) {
    EXPECT_EQ(Sha256::hash(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256, AbcMatchesPublishedVector) {
    EXPECT_EQ(Sha256::hash("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256, MultiBlockMatchesPublishedVector) {
    EXPECT_EQ(Sha256::hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256, LongerMultiBlockMatchesPublishedVector) {
    EXPECT_EQ(Sha256::hash("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                           "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
              "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST(Sha256, OneMillionAsMatchesPublishedVector) {
    Sha256 hasher;
    const std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) hasher.update(chunk);
    EXPECT_EQ(hasher.finalizeHex(),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256, StreamingInUnevenChunksMatchesSinglePass) {
    const std::string message = "TRACE evidence integrity, streamed in awkward pieces.";
    const std::string oneShot = Sha256::hash(message);

    Sha256 streamed;
    std::size_t offset = 0;
    for (std::size_t size : {1u, 7u, 13u, 2u, 31u, 5u}) {
        if (offset >= message.size()) break;
        streamed.update(message.data() + offset, std::min(size, message.size() - offset));
        offset += size;
    }
    if (offset < message.size()) streamed.update(message.data() + offset, message.size() - offset);
    EXPECT_EQ(streamed.finalizeHex(), oneShot);
}

TEST(Sha256, ExactBlockBoundaryLengths) {
    // 55, 56, 63, 64 and 65 bytes exercise every padding path.
    for (const std::size_t length : {55u, 56u, 63u, 64u, 65u, 119u, 128u}) {
        const std::string message(length, 'x');
        Sha256 chunked;
        chunked.update(message.data(), length / 2);
        chunked.update(message.data() + length / 2, length - length / 2);
        EXPECT_EQ(chunked.finalizeHex(), Sha256::hash(message))
            << "length " << length << " hashed differently when split";
    }
}

TEST(HashFile, MatchesInMemoryDigestAndReportsProgress) {
    testing::TemporaryDirectory directory("trace-hash");
    const auto file = directory / "payload.bin";
    const std::string contents(3 * 1024 * 1024 + 17, '\xA5');
    {
        std::ofstream out(file, std::ios::binary);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    std::int64_t lastReported = 0;
    auto hashed = hashFile(file, [&](const HashProgress& progress) {
        EXPECT_GE(progress.bytesProcessed, lastReported);
        lastReported = progress.bytesProcessed;
        EXPECT_EQ(progress.totalBytes, static_cast<std::int64_t>(contents.size()));
        return true;
    });

    ASSERT_TRUE(hashed.ok()) << hashed.error().toString();
    EXPECT_EQ(hashed.value(), Sha256::hash(contents));
    EXPECT_EQ(lastReported, static_cast<std::int64_t>(contents.size()));
}

TEST(HashFile, CancellationIsReportedAsCancelledNotSuccess) {
    testing::TemporaryDirectory directory("trace-hash-cancel");
    const auto file = directory / "payload.bin";
    {
        std::ofstream out(file, std::ios::binary);
        const std::string contents(4 * 1024 * 1024, 'z');
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    auto hashed = hashFile(file, [](const HashProgress&) { return false; });
    ASSERT_FALSE(hashed.ok());
    EXPECT_EQ(hashed.error().code(), ErrorCode::Cancelled);
}

TEST(HashFile, MissingFileIsNotFound) {
    auto hashed = hashFile("/definitely/not/here/evidence.mp4");
    ASSERT_FALSE(hashed.ok());
    EXPECT_EQ(hashed.error().code(), ErrorCode::NotFound);
}

}  // namespace
}  // namespace trace
