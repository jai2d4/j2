#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace trace {

/// Small, dependency-free JSON value used for the columns where TRACE keeps
/// semi-structured data: raw container/stream metadata, audit event details and
/// provenance parameters. Object keys keep insertion order so persisted JSON is
/// stable and diffable — important when a record is used as evidence of what
/// the software did.
class JsonValue {
public:
    enum class Type { Null, Bool, Int, Double, String, Array, Object };

    JsonValue() = default;
    JsonValue(bool value) : type_(Type::Bool), bool_(value) {}                    // NOLINT
    JsonValue(std::int64_t value) : type_(Type::Int), int_(value) {}              // NOLINT
    JsonValue(int value) : type_(Type::Int), int_(value) {}                       // NOLINT
    JsonValue(double value) : type_(Type::Double), double_(value) {}              // NOLINT
    JsonValue(std::string value) : type_(Type::String), string_(std::move(value)) {}  // NOLINT
    JsonValue(const char* value) : type_(Type::String), string_(value ? value : "") {} // NOLINT

    static JsonValue null() { return JsonValue(); }
    static JsonValue object();
    static JsonValue array();

    Type type() const noexcept { return type_; }
    bool isNull() const noexcept { return type_ == Type::Null; }
    bool isObject() const noexcept { return type_ == Type::Object; }
    bool isArray() const noexcept { return type_ == Type::Array; }

    /// Object mutation. Re-setting an existing key overwrites it in place.
    JsonValue& set(std::string key, JsonValue value);
    /// Convenience: skips the key entirely when the optional-ish value is absent,
    /// so "not available" is represented by omission rather than by a fake value.
    JsonValue& setIfNotEmpty(std::string key, const std::string& value);

    /// Array mutation.
    JsonValue& push(JsonValue value);

    /// Serialise. `indent < 0` produces compact output; otherwise pretty-printed.
    std::string dump(int indent = -1) const;

    const std::vector<std::pair<std::string, JsonValue>>& members() const { return members_; }
    const std::vector<JsonValue>& elements() const { return elements_; }
    const std::string& stringValue() const { return string_; }

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    std::int64_t int_ = 0;
    double double_ = 0.0;
    std::string string_;
    std::vector<JsonValue> elements_;
    std::vector<std::pair<std::string, JsonValue>> members_;
};

/// Escapes `text` as a JSON string literal (including the surrounding quotes).
/// Invalid UTF-8 byte sequences — which real-world container metadata does
/// contain — are replaced with U+FFFD so the emitted document stays valid.
std::string jsonEscape(const std::string& text);

}  // namespace trace
