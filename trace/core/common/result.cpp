#include "core/common/result.h"

namespace trace {

const char* toString(ErrorCode code) {
    switch (code) {
        case ErrorCode::None:             return "None";
        case ErrorCode::InvalidArgument:  return "InvalidArgument";
        case ErrorCode::NotFound:         return "NotFound";
        case ErrorCode::AlreadyExists:    return "AlreadyExists";
        case ErrorCode::PermissionDenied: return "PermissionDenied";
        case ErrorCode::IoError:          return "IoError";
        case ErrorCode::DatabaseError:    return "DatabaseError";
        case ErrorCode::MediaError:       return "MediaError";
        case ErrorCode::IntegrityFailure: return "IntegrityFailure";
        case ErrorCode::Unsupported:      return "Unsupported";
        case ErrorCode::Cancelled:        return "Cancelled";
        case ErrorCode::Internal:         return "Internal";
    }
    return "Unknown";
}

std::string Error::toString() const {
    std::string out = message_;
    if (!detail_.empty()) {
        out += " (";
        out += detail_;
        out += ")";
    }
    return out;
}

}  // namespace trace
