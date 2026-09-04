#pragma once

#include <string>

namespace trace {

enum class CaseStatus { Open, Active, OnHold, Closed, Archived };

const char* toString(CaseStatus status);
const char* toDisplayString(CaseStatus status);
CaseStatus caseStatusFromString(const std::string& text, CaseStatus fallback = CaseStatus::Open);

/// An investigation. Everything else in TRACE hangs off a case: evidence,
/// derived assets, annotations and the audit trail all carry its identifier.
struct Case {
    std::string id;              ///< UUID — stable identity
    std::string caseNumber;      ///< human-readable, e.g. CASE-0001
    std::string title;
    std::string description;
    CaseStatus status = CaseStatus::Open;
    std::string investigator;
    std::string agency;          ///< optional agency / client
    std::string location;        ///< optional incident location
    std::string notes;
    std::string createdAt;       ///< ISO-8601 UTC
    std::string modifiedAt;
    std::string createdBy;
    std::string storageRelPath;  ///< managed storage root, relative to the data directory
};

}  // namespace trace
