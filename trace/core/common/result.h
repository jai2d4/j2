#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace trace {

/// Coarse classification of a failure. UI layers map these onto user-facing
/// language; log/audit records keep the enum name so failures stay greppable.
enum class ErrorCode {
    None = 0,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    PermissionDenied,
    IoError,
    DatabaseError,
    MediaError,
    IntegrityFailure,
    Unsupported,
    Cancelled,
    Internal,
};

const char* toString(ErrorCode code);

/// A failure with a human-readable message plus optional technical detail
/// (an FFmpeg error string, an SQLite message, an errno description...).
class Error {
public:
    Error() = default;
    Error(ErrorCode code, std::string message, std::string detail = {})
        : code_(code), message_(std::move(message)), detail_(std::move(detail)) {}

    ErrorCode code() const noexcept { return code_; }
    const std::string& message() const noexcept { return message_; }
    const std::string& detail() const noexcept { return detail_; }

    /// "message (detail)" — used for logs and audit descriptions.
    std::string toString() const;

private:
    ErrorCode code_ = ErrorCode::Internal;
    std::string message_ = "unspecified error";
    std::string detail_;
};

/// Result<T> is TRACE's explicit success/failure channel. Domain code never
/// throws across module boundaries: an operation either yields a value or an
/// Error that the caller must inspect. This keeps evidence operations
/// fail-safe and forces callers to handle partial ingestion.
template <typename T>
class Result {
public:
    Result(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
    Result(Error error) : error_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

    static Result success(T value) { return Result(std::move(value)); }
    static Result failure(ErrorCode code, std::string message, std::string detail = {}) {
        return Result(Error(code, std::move(message), std::move(detail)));
    }

    bool ok() const noexcept { return value_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    const T& value() const& {
        if (!value_) throw std::logic_error("Result::value() on failed Result: " + error_.toString());
        return *value_;
    }
    T& value() & {
        if (!value_) throw std::logic_error("Result::value() on failed Result: " + error_.toString());
        return *value_;
    }
    T take() {
        if (!value_) throw std::logic_error("Result::take() on failed Result: " + error_.toString());
        return std::move(*value_);
    }
    T valueOr(T fallback) const { return value_ ? *value_ : std::move(fallback); }

    const Error& error() const noexcept { return error_; }

private:
    std::optional<T> value_;
    Error error_{ErrorCode::Internal, "uninitialised result"};
};

/// Result of an operation that yields no value.
class Status {
public:
    Status() = default;
    Status(Error error) : error_(std::move(error)), ok_(false) {}  // NOLINT(google-explicit-constructor)

    static Status success() { return {}; }
    static Status failure(ErrorCode code, std::string message, std::string detail = {}) {
        return Status(Error(code, std::move(message), std::move(detail)));
    }

    bool ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }
    const Error& error() const noexcept { return error_; }

private:
    Error error_{ErrorCode::None, ""};
    bool ok_ = true;
};

}  // namespace trace
