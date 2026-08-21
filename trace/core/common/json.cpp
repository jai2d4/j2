#include "core/common/json.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace trace {
namespace {

void appendIndent(std::string& out, int indent, int depth) {
    if (indent < 0) return;
    out.push_back('\n');
    out.append(static_cast<std::size_t>(indent * depth), ' ');
}

/// Returns the length of the valid UTF-8 sequence starting at `i`, or 0.
std::size_t utf8SequenceLength(const std::string& text, std::size_t i) {
    const auto byte = static_cast<unsigned char>(text[i]);
    std::size_t length = 0;
    if (byte < 0x80) return 1;
    if ((byte & 0xE0) == 0xC0) length = 2;
    else if ((byte & 0xF0) == 0xE0) length = 3;
    else if ((byte & 0xF8) == 0xF0) length = 4;
    else return 0;

    if (i + length > text.size()) return 0;
    for (std::size_t k = 1; k < length; ++k) {
        if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return 0;
    }
    return length;
}

std::string formatDouble(double value) {
    if (!std::isfinite(value)) return "null";
    std::array<char, 40> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%.10g", value);
    return std::string(buffer.data());
}

}  // namespace

std::string jsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        switch (c) {
            case '"':  out += "\\\""; ++i; continue;
            case '\\': out += "\\\\"; ++i; continue;
            case '\b': out += "\\b";  ++i; continue;
            case '\f': out += "\\f";  ++i; continue;
            case '\n': out += "\\n";  ++i; continue;
            case '\r': out += "\\r";  ++i; continue;
            case '\t': out += "\\t";  ++i; continue;
            default: break;
        }
        if (c < 0x20) {
            std::array<char, 8> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "\\u%04x", c);
            out += buffer.data();
            ++i;
            continue;
        }
        if (c < 0x80) {
            out.push_back(text[i]);
            ++i;
            continue;
        }
        const std::size_t length = utf8SequenceLength(text, i);
        if (length == 0) {
            out += "\xEF\xBF\xBD";  // U+FFFD REPLACEMENT CHARACTER
            ++i;
            continue;
        }
        out.append(text, i, length);
        i += length;
    }
    out.push_back('"');
    return out;
}

JsonValue JsonValue::object() {
    JsonValue value;
    value.type_ = Type::Object;
    return value;
}

JsonValue JsonValue::array() {
    JsonValue value;
    value.type_ = Type::Array;
    return value;
}

JsonValue& JsonValue::set(std::string key, JsonValue value) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        members_.clear();
    }
    for (auto& member : members_) {
        if (member.first == key) {
            member.second = std::move(value);
            return *this;
        }
    }
    members_.emplace_back(std::move(key), std::move(value));
    return *this;
}

JsonValue& JsonValue::setIfNotEmpty(std::string key, const std::string& value) {
    if (value.empty()) return *this;
    return set(std::move(key), JsonValue(value));
}

JsonValue& JsonValue::push(JsonValue value) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        elements_.clear();
    }
    elements_.push_back(std::move(value));
    return *this;
}

std::string JsonValue::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

void JsonValue::dumpTo(std::string& out, int indent, int depth) const {
    switch (type_) {
        case Type::Null:   out += "null"; return;
        case Type::Bool:   out += bool_ ? "true" : "false"; return;
        case Type::Int:    out += std::to_string(int_); return;
        case Type::Double: out += formatDouble(double_); return;
        case Type::String: out += jsonEscape(string_); return;
        case Type::Array: {
            if (elements_.empty()) { out += "[]"; return; }
            out.push_back('[');
            bool first = true;
            for (const auto& element : elements_) {
                if (!first) out.push_back(',');
                first = false;
                appendIndent(out, indent, depth + 1);
                element.dumpTo(out, indent, depth + 1);
            }
            appendIndent(out, indent, depth);
            out.push_back(']');
            return;
        }
        case Type::Object: {
            if (members_.empty()) { out += "{}"; return; }
            out.push_back('{');
            bool first = true;
            for (const auto& member : members_) {
                if (!first) out.push_back(',');
                first = false;
                appendIndent(out, indent, depth + 1);
                out += jsonEscape(member.first);
                out.push_back(':');
                if (indent >= 0) out.push_back(' ');
                member.second.dumpTo(out, indent, depth + 1);
            }
            appendIndent(out, indent, depth);
            out.push_back('}');
            return;
        }
    }
}

}  // namespace trace
