#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/database/database.h"
#include "core/models/report.h"

namespace trace {

/// Persistence for reports and the items an operator selected into them.
///
/// The terminal-state rule matches analysis runs: a report that carries an error is
/// never written as `exported`, and a non-terminal state is never written as a terminal
/// one. Enforced here rather than in the service, so a future caller cannot bypass it.
class ReportRepository {
public:
    explicit ReportRepository(std::shared_ptr<Database> database);

    Status insertReport(const Report& report);
    /// Inserts every item in one transaction, reusing a single prepared statement.
    Status insertItems(const std::vector<ReportItem>& items);
    /// Records the derived asset a frame or clip item finally produced.
    Status attachDerivedAsset(const std::string& itemId, const std::string& derivedAssetId);

    Status markExportStarted(const std::string& reportId, const std::string& startedAt);
    /// Writes the terminal state. Refuses `exported` while carrying an error, and
    /// refuses any status that is not terminal.
    Status finishExport(const std::string& reportId, ReportStatus status,
                        const std::string& completedAt,
                        const std::optional<std::string>& bundleRelPath = std::nullopt,
                        const std::optional<std::string>& manifestSha256 = std::nullopt,
                        const std::string& itemCountsJson = "{}",
                        const std::optional<std::string>& errorMessage = std::nullopt);

    Result<std::optional<Report>> findReport(const std::string& reportId);
    Result<std::vector<Report>> listForCase(const std::string& caseId);
    Result<std::vector<ReportItem>> itemsFor(const std::string& reportId);
    Result<std::int64_t> countForCase(const std::string& caseId);

private:
    std::shared_ptr<Database> database_;
};

}  // namespace trace
