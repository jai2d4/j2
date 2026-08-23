#include "core/repositories/report_repository.h"

#include <utility>

namespace trace {
namespace {

constexpr const char* kReportColumns =
    "id, case_id, title, status, bundle_relpath, manifest_sha256, trace_version, "
    "export_started_at, export_completed_at, item_counts_json, included_unconfirmed, "
    "error_message, created_by, created_at";

constexpr const char* kItemColumns =
    "id, report_id, item_type, evidence_id, detection_id, bookmark_id, annotation_id, "
    "derived_asset_id, timestamp_us, range_start_us, range_end_us, caption, sort_order, "
    "created_at";

Report readReport(const Statement& stmt) {
    Report report;
    report.id = stmt.columnText(0);
    report.caseId = stmt.columnText(1);
    report.title = stmt.columnText(2);
    report.status = reportStatusFromString(stmt.columnText(3));
    report.bundleRelPath = stmt.columnOptionalText(4);
    report.manifestSha256 = stmt.columnOptionalText(5);
    report.traceVersion = stmt.columnText(6);
    report.exportStartedAt = stmt.columnOptionalText(7);
    report.exportCompletedAt = stmt.columnOptionalText(8);
    report.itemCountsJson = stmt.columnText(9);
    report.includedUnconfirmed = stmt.columnInt64(10) != 0;
    report.errorMessage = stmt.columnOptionalText(11);
    report.createdBy = stmt.columnText(12);
    report.createdAt = stmt.columnText(13);
    return report;
}

ReportItem readItem(const Statement& stmt) {
    ReportItem item;
    item.id = stmt.columnText(0);
    item.reportId = stmt.columnText(1);
    item.type = reportItemTypeFromString(stmt.columnText(2));
    item.evidenceId = stmt.columnOptionalText(3);
    item.detectionId = stmt.columnOptionalText(4);
    item.bookmarkId = stmt.columnOptionalText(5);
    item.annotationId = stmt.columnOptionalText(6);
    item.derivedAssetId = stmt.columnOptionalText(7);
    item.timestampUs = stmt.columnOptionalInt64(8);
    item.rangeStartUs = stmt.columnOptionalInt64(9);
    item.rangeEndUs = stmt.columnOptionalInt64(10);
    item.caption = stmt.columnText(11);
    item.sortOrder = static_cast<int>(stmt.columnInt64(12));
    item.createdAt = stmt.columnText(13);
    return item;
}

}  // namespace

ReportRepository::ReportRepository(std::shared_ptr<Database> database)
    : database_(std::move(database)) {}

Status ReportRepository::insertReport(const Report& report) {
    auto prepared = database_->prepare(std::string("INSERT INTO reports (") + kReportColumns +
                                       ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!prepared) return Status(prepared.error());

    Statement stmt = prepared.take();
    stmt.bind(1, report.id)
        .bind(2, report.caseId)
        .bind(3, report.title)
        .bind(4, std::string(toString(report.status)))
        .bindOptional(5, report.bundleRelPath)
        .bindOptional(6, report.manifestSha256)
        .bind(7, report.traceVersion)
        .bindOptional(8, report.exportStartedAt)
        .bindOptional(9, report.exportCompletedAt)
        .bind(10, report.itemCountsJson)
        .bind(11, report.includedUnconfirmed ? 1 : 0)
        .bindOptional(12, report.errorMessage)
        .bind(13, report.createdBy)
        .bind(14, report.createdAt);
    return stmt.run();
}

Status ReportRepository::insertItems(const std::vector<ReportItem>& items) {
    if (items.empty()) return Status::success();

    Transaction transaction(*database_);
    if (auto status = transaction.begin(); !status) return status;

    auto prepared = database_->prepare(std::string("INSERT INTO report_items (") + kItemColumns +
                                       ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();

    for (const auto& item : items) {
        if (auto status = stmt.reset(); !status) return status;
        stmt.bind(1, item.id)
            .bind(2, item.reportId)
            .bind(3, std::string(toString(item.type)))
            .bindOptional(4, item.evidenceId)
            .bindOptional(5, item.detectionId)
            .bindOptional(6, item.bookmarkId)
            .bindOptional(7, item.annotationId)
            .bindOptional(8, item.derivedAssetId)
            .bindOptional(9, item.timestampUs)
            .bindOptional(10, item.rangeStartUs)
            .bindOptional(11, item.rangeEndUs)
            .bind(12, item.caption)
            .bind(13, item.sortOrder)
            .bind(14, item.createdAt);
        if (auto status = stmt.run(); !status) return status;
    }
    return transaction.commit();
}

Status ReportRepository::attachDerivedAsset(const std::string& itemId,
                                            const std::string& derivedAssetId) {
    auto prepared =
        database_->prepare("UPDATE report_items SET derived_asset_id = ? WHERE id = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, derivedAssetId).bind(2, itemId);
    if (auto status = stmt.run(); !status) return status;
    if (database_->changes() == 0) {
        return Status::failure(ErrorCode::NotFound, "Report item not found: " + itemId);
    }
    return Status::success();
}

Status ReportRepository::markExportStarted(const std::string& reportId,
                                           const std::string& startedAt) {
    auto prepared = database_->prepare(
        "UPDATE reports SET status = 'exporting', export_started_at = ?, error_message = NULL "
        "WHERE id = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, startedAt).bind(2, reportId);
    if (auto status = stmt.run(); !status) return status;
    if (database_->changes() == 0) {
        return Status::failure(ErrorCode::NotFound, "Report not found: " + reportId);
    }
    return Status::success();
}

Status ReportRepository::finishExport(const std::string& reportId, ReportStatus status,
                                      const std::string& completedAt,
                                      const std::optional<std::string>& bundleRelPath,
                                      const std::optional<std::string>& manifestSha256,
                                      const std::string& itemCountsJson,
                                      const std::optional<std::string>& errorMessage) {
    // The two rules that keep an export honest, enforced at the storage boundary.
    if (!isTerminal(status)) {
        return Status::failure(ErrorCode::InvalidArgument,
                               std::string("An export cannot finish in a non-terminal state: ") +
                                   toString(status));
    }
    if (status == ReportStatus::Exported && errorMessage.has_value() &&
        !errorMessage->empty()) {
        return Status::failure(
            ErrorCode::InvalidArgument,
            "An export that carries an error is never recorded as exported");
    }
    if (status == ReportStatus::Exported && (!bundleRelPath || !manifestSha256)) {
        return Status::failure(
            ErrorCode::InvalidArgument,
            "An exported report must record where its bundle is and the digest of its manifest");
    }

    auto prepared = database_->prepare(
        "UPDATE reports SET status = ?, export_completed_at = ?, bundle_relpath = ?, "
        "manifest_sha256 = ?, item_counts_json = ?, error_message = ? WHERE id = ?;");
    if (!prepared) return Status(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, std::string(toString(status)))
        .bind(2, completedAt)
        .bindOptional(3, bundleRelPath)
        .bindOptional(4, manifestSha256)
        .bind(5, itemCountsJson)
        .bindOptional(6, errorMessage)
        .bind(7, reportId);
    if (auto runStatus = stmt.run(); !runStatus) return runStatus;
    if (database_->changes() == 0) {
        return Status::failure(ErrorCode::NotFound, "Report not found: " + reportId);
    }
    return Status::success();
}

Result<std::optional<Report>> ReportRepository::findReport(const std::string& reportId) {
    using ResultType = Result<std::optional<Report>>;
    auto prepared = database_->prepare(std::string("SELECT ") + kReportColumns +
                                       " FROM reports WHERE id = ?;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, reportId);
    auto stepped = stmt.step();
    if (!stepped) return ResultType(stepped.error());
    if (!stepped.value()) return ResultType::success(std::nullopt);
    return ResultType::success(readReport(stmt));
}

Result<std::vector<Report>> ReportRepository::listForCase(const std::string& caseId) {
    using ResultType = Result<std::vector<Report>>;
    auto prepared = database_->prepare(std::string("SELECT ") + kReportColumns +
                                       " FROM reports WHERE case_id = ? ORDER BY created_at DESC;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, caseId);

    std::vector<Report> reports;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.value()) break;
        reports.push_back(readReport(stmt));
    }
    return ResultType::success(std::move(reports));
}

Result<std::vector<ReportItem>> ReportRepository::itemsFor(const std::string& reportId) {
    using ResultType = Result<std::vector<ReportItem>>;
    auto prepared =
        database_->prepare(std::string("SELECT ") + kItemColumns +
                           " FROM report_items WHERE report_id = ? ORDER BY sort_order, id;");
    if (!prepared) return ResultType(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, reportId);

    std::vector<ReportItem> items;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) return ResultType(stepped.error());
        if (!stepped.value()) break;
        items.push_back(readItem(stmt));
    }
    return ResultType::success(std::move(items));
}

Result<std::int64_t> ReportRepository::countForCase(const std::string& caseId) {
    auto prepared = database_->prepare("SELECT COUNT(*) FROM reports WHERE case_id = ?;");
    if (!prepared) return Result<std::int64_t>(prepared.error());
    Statement stmt = prepared.take();
    stmt.bind(1, caseId);
    auto stepped = stmt.step();
    if (!stepped) return Result<std::int64_t>(stepped.error());
    return Result<std::int64_t>::success(stepped.value() ? stmt.columnInt64(0) : 0);
}

}  // namespace trace
