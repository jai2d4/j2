// Managed storage paths: identity comes from UUIDs, not from filenames, and
// nothing a user types can escape the case directory.
#include <gtest/gtest.h>

#include <fstream>

#include "core/common/uuid.h"
#include "core/security/sha256.h"
#include "core/storage/file_operations.h"
#include "core/storage/storage_layout.h"
#include "tests/support/test_environment.h"

namespace trace {
namespace {

TEST(StorageLayout, BuildsTheDocumentedDirectoryShape) {
    testing::TemporaryDirectory directory("trace-layout");
    StorageLayout layout(directory.path());
    const std::string caseId = generateUuid();

    ASSERT_TRUE(layout.ensureCaseDirectories(caseId).ok());
    EXPECT_TRUE(std::filesystem::exists(layout.originalsDirectory(caseId)));
    EXPECT_TRUE(std::filesystem::exists(layout.workingDirectory(caseId)));
    EXPECT_TRUE(std::filesystem::exists(layout.thumbnailsDirectory(caseId)));
    EXPECT_TRUE(std::filesystem::exists(layout.exportsDirectory(caseId)));
    EXPECT_TRUE(std::filesystem::exists(layout.reportsDirectory(caseId)));
    EXPECT_TRUE(std::filesystem::exists(layout.caseLogsDirectory(caseId)));
    EXPECT_TRUE(std::filesystem::exists(layout.logsDirectory()));
    EXPECT_EQ(layout.databasePath(), directory.path() / "trace.db");
    EXPECT_EQ(StorageLayout::caseRelPath(caseId), "cases/" + caseId);
}

TEST(StorageLayout, StoresRelativePathsSoDataDirectoriesCanMove) {
    testing::TemporaryDirectory directory("trace-layout-rel");
    StorageLayout layout(directory.path());
    const std::string caseId = generateUuid();
    const auto absolute = layout.originalsDirectory(caseId) / "abc_clip.mp4";

    const std::string relative = layout.relativeTo(absolute);
    EXPECT_EQ(relative, "cases/" + caseId + "/originals/abc_clip.mp4");
    EXPECT_EQ(layout.resolve(relative), absolute);

    // The same relative path resolves under a different root.
    StorageLayout moved(directory.path() / "elsewhere");
    EXPECT_EQ(moved.resolve(relative), directory.path() / "elsewhere" / relative);
}

TEST(StorageLayout, ManagedNamesAreCollisionSafeAndReadable) {
    const std::string first = generateUuid();
    const std::string second = generateUuid();

    const std::string a = StorageLayout::managedOriginalFilename(first, "body cam.MP4");
    const std::string b = StorageLayout::managedOriginalFilename(second, "body cam.MP4");
    EXPECT_NE(a, b) << "identical source names must not collide in managed storage";
    EXPECT_NE(a.find("body cam"), std::string::npos);
    EXPECT_NE(a.find(".MP4"), std::string::npos);
    EXPECT_TRUE(a.rfind(uuidToCompact(first), 0) == 0);

    const std::string hostile =
        StorageLayout::managedOriginalFilename(generateUuid(), "../../../etc/shadow");
    EXPECT_EQ(hostile.find('/'), std::string::npos);
    EXPECT_EQ(hostile.find('\\'), std::string::npos);
}

TEST(StorageLayout, DerivedNamesCarryEvidenceAndTimecode) {
    const std::string name = StorageLayout::derivedFilename("EVD-000001", "frame", "00-00-05-000",
                                                            ".png", generateUuid());
    EXPECT_NE(name.find("EVD-000001"), std::string::npos);
    EXPECT_NE(name.find("frame"), std::string::npos);
    EXPECT_NE(name.find("00-00-05-000"), std::string::npos);
    EXPECT_TRUE(name.size() > 4 && name.substr(name.size() - 4) == ".png");
}

TEST(FileOperations, CopyIsByteIdenticalAndIndependentlyVerified) {
    testing::TemporaryDirectory directory("trace-copy");
    const auto source = directory / "source.bin";
    const std::string payload = std::string(2 * 1024 * 1024, '\x5C') + "tail";
    {
        std::ofstream out(source, std::ios::binary);
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }

    const auto destination = directory / "managed" / "copy.bin";
    auto copied = copyIntoManagedStorage(source, destination);
    ASSERT_TRUE(copied.ok()) << copied.error().toString();
    EXPECT_EQ(copied.value().bytesCopied, static_cast<std::int64_t>(payload.size()));
    EXPECT_EQ(copied.value().sourceSha256, Sha256::hash(payload));
    EXPECT_EQ(copied.value().destinationSha256, copied.value().sourceSha256);
    EXPECT_EQ(std::filesystem::file_size(destination), payload.size());
    EXPECT_TRUE(std::filesystem::exists(source)) << "the source must never be consumed";
}

TEST(FileOperations, RefusesToOverwriteSomethingAlreadyInManagedStorage) {
    testing::TemporaryDirectory directory("trace-copy-guard");
    const auto source = directory / "source.bin";
    const auto destination = directory / "existing.bin";
    { std::ofstream(source, std::ios::binary) << "new"; }
    { std::ofstream(destination, std::ios::binary) << "already stored evidence"; }

    auto copied = copyIntoManagedStorage(source, destination);
    ASSERT_FALSE(copied.ok());
    EXPECT_EQ(copied.error().code(), ErrorCode::AlreadyExists);

    std::ifstream check(destination);
    std::string contents;
    std::getline(check, contents);
    EXPECT_EQ(contents, "already stored evidence") << "existing evidence was overwritten";
}

TEST(FileOperations, CancellationRemovesThePartialCopy) {
    testing::TemporaryDirectory directory("trace-copy-cancel");
    const auto source = directory / "source.bin";
    {
        std::ofstream out(source, std::ios::binary);
        const std::string payload(4 * 1024 * 1024, 'q');
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }
    const auto destination = directory / "managed" / "copy.bin";

    auto copied = copyIntoManagedStorage(source, destination,
                                         [](const HashProgress&) { return false; });
    ASSERT_FALSE(copied.ok());
    EXPECT_EQ(copied.error().code(), ErrorCode::Cancelled);
    EXPECT_FALSE(std::filesystem::exists(destination));
}

}  // namespace
}  // namespace trace
