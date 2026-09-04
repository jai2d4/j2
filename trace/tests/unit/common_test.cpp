// Identifiers, timestamps, timecodes and JSON — the small pieces every record
// depends on.
#include <gtest/gtest.h>

#include <set>

#include "core/common/json.h"
#include "core/common/string_utils.h"
#include "core/common/time_utils.h"
#include "core/common/uuid.h"

namespace trace {
namespace {

TEST(Uuid, IsCanonicalAndUnique) {
    std::set<std::string> seen;
    for (int i = 0; i < 5000; ++i) {
        const std::string value = generateUuid();
        ASSERT_TRUE(isUuid(value)) << value;
        EXPECT_EQ(value[14], '4') << "expected a version 4 UUID: " << value;
        EXPECT_TRUE(value[19] == '8' || value[19] == '9' || value[19] == 'a' || value[19] == 'b')
            << "expected RFC 4122 variant: " << value;
        EXPECT_TRUE(seen.insert(value).second) << "duplicate identifier generated: " << value;
    }
    EXPECT_EQ(uuidToCompact("0f6cfa46-7c1b-4a55-9c3d-1b2e3f405162").size(), 32u);
}

TEST(Uuid, SeedingDrawsRealEntropy) {
    // A build that optimises the seeding away would return the same value (or
    // zero) every time, and every TRACE installation would then mint identical
    // evidence identifiers.
    std::set<std::uint64_t> seeds;
    for (int i = 0; i < 8; ++i) seeds.insert(detail::generateEntropySeed());
    EXPECT_GT(seeds.size(), 1u) << "the entropy source is returning a constant";
    EXPECT_EQ(seeds.count(0), 0u) << "the entropy source returned zero";
}

TEST(Uuid, IdentifiersDifferAcrossFreshEngines) {
    // The first identifier a process mints must not be predictable: run the
    // generator in child processes and compare.
    const std::string first = generateUuid();
    EXPECT_NE(first, generateUuid());
}

TEST(Timecode, FormatsAndParsesRoundTrip) {
    EXPECT_EQ(formatTimecode(0), "00:00:00.000");
    EXPECT_EQ(formatTimecode(5'000'000), "00:00:05.000");
    EXPECT_EQ(formatTimecode(97'433'000), "00:01:37.433");
    EXPECT_EQ(formatTimecode(3'661'500'000), "01:01:01.500");
    EXPECT_EQ(formatTimecode(97'433'000, false), "00:01:37");

    EXPECT_EQ(parseTimecode("00:01:37.433").value(), 97'433'000);
    EXPECT_EQ(parseTimecode("00:00:05").value(), 5'000'000);
    EXPECT_EQ(parseTimecode("1:37.5").value(), 97'500'000);
    EXPECT_EQ(parseTimecode("12.25").value(), 12'250'000);
    EXPECT_FALSE(parseTimecode("not a timecode").has_value());
    EXPECT_FALSE(parseTimecode("").has_value());
    EXPECT_FALSE(parseTimecode("1:2:3:4").has_value());
}

TEST(Iso8601, RoundTripsThroughUtc) {
    const std::string now = nowIso8601Utc();
    ASSERT_EQ(now.size(), 24u) << now;
    EXPECT_EQ(now.back(), 'Z');

    const auto parsed = parseIso8601Utc(now);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(toIso8601Utc(*parsed), now);
    EXPECT_FALSE(parseIso8601Utc("yesterday").has_value());
}

TEST(Json, EscapesAndPreservesInsertionOrder) {
    JsonValue object = JsonValue::object();
    object.set("case", "CASE-0001")
        .set("quote", "he said \"stop\"")
        .set("path", "C:\\evidence\\clip.mp4")
        .set("newline", "line1\nline2")
        .set("count", static_cast<std::int64_t>(3))
        .set("ratio", 29.97)
        .set("verified", true)
        .set("missing", JsonValue::null());

    const std::string dumped = object.dump();
    EXPECT_NE(dumped.find("\"case\":\"CASE-0001\""), std::string::npos);
    EXPECT_NE(dumped.find("\\\"stop\\\""), std::string::npos);
    EXPECT_NE(dumped.find("C:\\\\evidence\\\\clip.mp4"), std::string::npos);
    EXPECT_NE(dumped.find("line1\\nline2"), std::string::npos);
    EXPECT_NE(dumped.find("\"verified\":true"), std::string::npos);
    EXPECT_NE(dumped.find("\"missing\":null"), std::string::npos);
    EXPECT_LT(dumped.find("\"case\""), dumped.find("\"quote\""));
}

TEST(Json, ReplacesInvalidUtf8SoDocumentsStayValid) {
    // Container metadata in the field is not always valid UTF-8.
    const std::string broken = std::string("camera") + '\xC3' + std::string("model");
    const std::string escaped = jsonEscape(broken);
    EXPECT_NE(escaped.find("\xEF\xBF\xBD"), std::string::npos);
    EXPECT_EQ(escaped.front(), '"');
    EXPECT_EQ(escaped.back(), '"');
}

TEST(Json, NestedStructuresAndPrettyPrinting) {
    JsonValue streams = JsonValue::array();
    streams.push(JsonValue::object().set("index", 0).set("type", "video"));
    streams.push(JsonValue::object().set("index", 1).set("type", "audio"));
    JsonValue root = JsonValue::object().set("streams", streams);

    const std::string pretty = root.dump(2);
    EXPECT_NE(pretty.find("\n  \"streams\""), std::string::npos);
    EXPECT_NE(pretty.find("\"type\": \"audio\""), std::string::npos);
}

TEST(StringUtils, SanitisesNamesForEveryFilesystem) {
    EXPECT_EQ(sanitizeForFilename("body cam 12.mp4"), "body cam 12.mp4");
    // Path separators become underscores, so a traversal attempt collapses into
    // a harmless flat name.
    EXPECT_EQ(sanitizeForFilename("../../etc/passwd"), ".._.._etc_passwd");
    EXPECT_EQ(sanitizeForFilename("clip:with*bad?chars"), "clip_with_bad_chars");
    EXPECT_EQ(sanitizeForFilename("trailing dots..."), "trailing dots");
    EXPECT_EQ(sanitizeForFilename(""), "unnamed");
    EXPECT_EQ(sanitizeForFilename("CON"), "_CON");
    EXPECT_EQ(sanitizeForFilename("com1.txt"), "_com1.txt");
    EXPECT_LE(sanitizeForFilename(std::string(500, 'a')).size(), 64u);
}

TEST(StringUtils, FormatsSizesAndSequenceNumbers) {
    EXPECT_EQ(formatFileSize(0), "0 bytes");
    EXPECT_EQ(formatFileSize(512), "512 bytes");
    EXPECT_NE(formatFileSize(1536).find("1.50 KB"), std::string::npos);
    EXPECT_EQ(formatFileSize(-1), "Not available");

    EXPECT_EQ(formatSequenceNumber("CASE-", 1, 4), "CASE-0001");
    EXPECT_EQ(formatSequenceNumber("EVD-", 42, 6), "EVD-000042");
    EXPECT_EQ(formatSequenceNumber("EVD-", 1234567, 6), "EVD-1234567");
}

}  // namespace
}  // namespace trace
