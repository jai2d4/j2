#include "reporting/report_service.h"

#include <algorithm>
#include <system_error>

#include "core/common/logging.h"
#include "core/common/time_utils.h"
#include "core/common/uuid.h"
#include "core/security/user_context.h"
#include "media/ffmpeg/video_decoder.h"
#include "trace/trace_version.h"

namespace trace {
namespace {

constexpr const char* kComponent = "reporting";

std::string sanitiseForFolder(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '-';
    }
    return out;
}

std::string timecodeForFilename(Microseconds positionUs) {
    std::string text = formatTimecode(positionUs);
    for (char& c : text) {
        if (c == ':' || c == '.') c = '-';
    }
    return text;
}

}  // namespace

ReportService::ReportService(StorageLayout layout, std::shared_ptr<Database> database,
                             std::shared_ptr<AuditService> audit, CaseService& cases,
                             EvidenceService& evidence, AnalysisService& analysis,
                             AnnotationService& annotations,
                             std::shared_ptr<DerivedAssetService> derivedAssets)
    : layout_(std::move(layout)),
      reports_(std::make_shared<ReportRepository>(std::move(database))),
      audit_(std::move(audit)),
      cases_(cases),
      evidence_(evidence),
      analysis_(analysis),
      annotations_(annotations),
      derivedAssets_(std::move(derivedAssets)),
      clips_(layout_, derivedAssets_),
      frames_(layout_, derivedAssets_) {}

std::string ReportService::bundleFolderName(const std::string& caseNumber,
                                            const std::string& exportedAtUtc) {
    return sanitiseForFolder(caseNumber) + "_exhibit_" + sanitiseForFolder(exportedAtUtc);
}

Result<Report> ReportService::createReport(const ReportDraft& draft) {
    using ResultType = Result<Report>;

    if (draft.caseId.empty()) {
        return ResultType::failure(ErrorCode::InvalidArgument, "A report needs a case");
    }
    if (draft.title.empty()) {
        return ResultType::failure(ErrorCode::InvalidArgument, "A report needs a title");
    }
    if (draft.evidenceIds.empty()) {
        return ResultType::failure(ErrorCode::InvalidArgument,
                                   "A report needs at least one evidence item");
    }

    Report report;
    report.id = generateUuid();
    report.caseId = draft.caseId;
    report.title = draft.title;
    report.status = ReportStatus::Draft;
    report.traceVersion = kApplicationVersion;
    report.includedUnconfirmed = draft.includeUnconfirmedDetections;
    report.createdBy = UserContext::current().actorName();
    report.createdAt = nowIso8601Utc();

    int order = 0;
    const std::string now = report.createdAt;
    const auto makeItem = [&](ReportItemType type) {
        ReportItem item;
        item.id = generateUuid();
        item.reportId = report.id;
        item.type = type;
        item.sortOrder = order++;
        item.createdAt = now;
        return item;
    };

    for (const auto& id : draft.evidenceIds) {
        auto item = makeItem(ReportItemType::Evidence);
        item.evidenceId = id;
        report.items.push_back(std::move(item));
    }
    for (const auto& id : draft.detectionIds) {
        auto item = makeItem(ReportItemType::Detection);
        item.detectionId = id;
        report.items.push_back(std::move(item));
    }
    for (const auto& frame : draft.frames) {
        auto item = makeItem(ReportItemType::Frame);
        item.evidenceId = frame.evidenceId;
        item.timestampUs = frame.timestampUs;
        item.caption = frame.caption;
        report.items.push_back(std::move(item));
    }
    for (const auto& clip : draft.clips) {
        auto item = makeItem(ReportItemType::Clip);
        item.evidenceId = clip.evidenceId;
        item.rangeStartUs = clip.startUs;
        item.rangeEndUs = clip.endUs;
        item.caption = clip.caption;
        report.items.push_back(std::move(item));
    }
    for (const auto& id : draft.bookmarkIds) {
        auto item = makeItem(ReportItemType::Bookmark);
        item.bookmarkId = id;
        report.items.push_back(std::move(item));
    }
    for (const auto& id : draft.annotationIds) {
        auto item = makeItem(ReportItemType::Annotation);
        item.annotationId = id;
        report.items.push_back(std::move(item));
    }

    if (auto status = reports_->insertReport(report); !status) return ResultType(status.error());
    if (auto status = reports_->insertItems(report.items); !status) return ResultType(status.error());

    auto caseRecord = cases_.findById(draft.caseId);
    AuditRecord record;
    record.action = AuditAction::ReportCreated;
    record.caseId = draft.caseId;
    if (caseRecord && caseRecord.value()) record.caseNumber = caseRecord.value()->caseNumber;
    record.description = "Report drafted: " + report.title;
    record.details = JsonValue::object()
                         .set("report_id", report.id)
                         .set("items", static_cast<std::int64_t>(report.items.size()))
                         .set("includes_unconfirmed_detections", report.includedUnconfirmed);
    audit_->record(record);

    return ResultType::success(std::move(report));
}

Result<ExportOutcome> ReportService::exportReport(
    const std::string& reportId, const std::filesystem::path& destinationRoot,
    const ExportProgressCallback& progress, std::shared_ptr<std::atomic<bool>> cancellation) {
    using ResultType = Result<ExportOutcome>;

    ExportOutcome outcome;

    auto found = reports_->findReport(reportId);
    if (!found) return ResultType(found.error());
    if (!found.value()) {
        return ResultType::failure(ErrorCode::NotFound, "Report not found: " + reportId);
    }
    outcome.report = *found.take();

    auto itemsResult = reports_->itemsFor(reportId);
    if (!itemsResult) return ResultType(itemsResult.error());
    outcome.report.items = itemsResult.take();

    auto caseResult = cases_.findById(outcome.report.caseId);
    if (!caseResult || !caseResult.value()) {
        return ResultType::failure(ErrorCode::NotFound, "The report's case no longer exists");
    }
    const Case caseRecord = *caseResult.take();

    const std::string exportedAt = nowIso8601Utc();
    const std::string exportedBy = UserContext::current().actorName();
    outcome.bundlePath = destinationRoot / bundleFolderName(caseRecord.caseNumber, exportedAt);

    // Everything below can fail. One place records the failure, so no path can leave a
    // report claiming an export it did not complete.
    const auto fail = [&](const std::string& message) {
        reports_->finishExport(reportId, ReportStatus::Failed, nowIso8601Utc(), std::nullopt,
                               std::nullopt, "{}", message);
        AuditRecord record;
        record.action = AuditAction::ReportExportFailed;
        record.caseId = caseRecord.id;
        record.caseNumber = caseRecord.caseNumber;
        record.description = "Report export failed: " + message;
        record.outcome = "failure";
        record.details = JsonValue::object().set("report_id", reportId).set("error", message);
        audit_->record(record);

        std::error_code ec;
        std::filesystem::remove_all(outcome.bundlePath, ec);

        outcome.status = ReportStatus::Failed;
        outcome.error = message;
        return ResultType::success(outcome);
    };

    const auto cancelled = [&] { return cancellation != nullptr && cancellation->load(); };
    const auto cancel = [&]() {
        reports_->finishExport(reportId, ReportStatus::Cancelled, nowIso8601Utc(), std::nullopt,
                               std::nullopt, "{}", std::nullopt);
        AuditRecord record;
        record.action = AuditAction::ReportExportCancelled;
        record.caseId = caseRecord.id;
        record.caseNumber = caseRecord.caseNumber;
        record.description = "Report export cancelled by the operator";
        record.details = JsonValue::object().set("report_id", reportId);
        audit_->record(record);

        std::error_code ec;
        std::filesystem::remove_all(outcome.bundlePath, ec);
        outcome.status = ReportStatus::Cancelled;
        return ResultType::success(outcome);
    };

    if (auto status = reports_->markExportStarted(reportId, exportedAt); !status) {
        return ResultType(status.error());
    }

    int step = 0;
    const int totalSteps = static_cast<int>(outcome.report.items.size()) + 6;
    const auto advance = [&](const std::string& stage) {
        ExportProgress update;
        update.completedSteps = ++step;
        update.totalSteps = totalSteps;
        update.stage = stage;
        if (progress && !progress(update)) {
            if (cancellation != nullptr) cancellation->store(true);
        }
    };

    BundleWriter writer(outcome.bundlePath);
    if (auto status = writer.createLayout(); !status) return fail(status.error().message());

    // ------------------------------------------------- gather the records
    ReportContent content;
    content.caseRecord = caseRecord;
    content.report = outcome.report;
    content.exportedAtUtc = exportedAt;
    content.exportedBy = exportedBy;
    content.traceVersion = kApplicationVersion;

    std::vector<std::string> evidenceIds;
    for (const auto& item : outcome.report.items) {
        if (item.type != ReportItemType::Evidence || !item.evidenceId) continue;
        evidenceIds.push_back(*item.evidenceId);
        auto record = evidence_.findById(*item.evidenceId);
        if (!record || !record.value()) return fail("An evidence item in this report is missing");
        content.evidence.push_back(*record.take());

        auto metadata = evidence_.metadataFor(*item.evidenceId);
        content.evidenceMetadata.push_back(metadata ? metadata.take()
                                                    : std::optional<MediaMetadata>{});
        advance("Reading evidence records");
    }
    if (content.evidence.empty()) return fail("This report contains no evidence items");

    const auto evidenceNumberFor = [&](const std::string& id) {
        for (const auto& e : content.evidence) {
            if (e.id == id) return e.evidenceNumber;
        }
        return std::string("(not in this report)");
    };
    const auto evidenceFor = [&](const std::string& id) -> const Evidence* {
        for (const auto& e : content.evidence) {
            if (e.id == id) return &e;
        }
        return nullptr;
    };

    for (const auto& item : outcome.report.items) {
        if (item.type != ReportItemType::Detection || !item.detectionId) continue;
        auto detection = analysis_.findDetection(*item.detectionId);
        if (!detection || !detection.value()) {
            return fail("A detection cited by this report no longer exists");
        }
        CitedDetection cited;
        cited.detection = *detection.take();

        auto run = analysis_.findRun(cited.detection.analysisRunId);
        if (!run || !run.value()) {
            return fail("The analysis run behind a cited detection no longer exists");
        }
        cited.run = *run.take();
        cited.evidenceNumber = evidenceNumberFor(cited.detection.evidenceId);

        // The confirmed-only rule, enforced where it cannot be bypassed by a caller.
        if (!outcome.report.includedUnconfirmed &&
            cited.detection.verification != DetectionVerification::Confirmed) {
            return fail(
                "This report cites a detection no analyst has confirmed, but it was not created "
                "with unconfirmed detections enabled");
        }
        content.detections.push_back(std::move(cited));
        advance("Reading detections");
    }

    for (const auto& item : outcome.report.items) {
        if (item.type == ReportItemType::Bookmark && item.bookmarkId) {
            for (const auto& id : evidenceIds) {
                auto list = annotations_.bookmarksForEvidence(id);
                if (!list) continue;
                for (const auto& bookmark : list.value()) {
                    if (bookmark.id == *item.bookmarkId) content.bookmarks.push_back(bookmark);
                }
            }
            advance("Reading bookmarks");
        } else if (item.type == ReportItemType::Annotation && item.annotationId) {
            for (const auto& id : evidenceIds) {
                auto list = annotations_.annotationsForEvidence(id);
                if (!list) continue;
                for (const auto& annotation : list.value()) {
                    if (annotation.id == *item.annotationId) {
                        content.annotations.push_back(annotation);
                    }
                }
            }
            advance("Reading notes");
        }
    }

    if (cancelled()) return cancel();

    // ------------------------------------------- produce frames and clips
    for (const auto& item : outcome.report.items) {
        if (cancelled()) return cancel();

        if (item.type == ReportItemType::Frame && item.evidenceId && item.timestampUs) {
            const Evidence* source = evidenceFor(*item.evidenceId);
            if (source == nullptr) return fail("A requested frame refers to evidence not included");

            auto sourceKey = evidence_.caseKey(source->caseId);
            if (!sourceKey) return fail(sourceKey.error().message());
            const CaseKeyHandle key = sourceKey.take();

            auto opened = VideoDecoder::open(evidence_.absolutePath(*source), key.get());
            if (!opened) return fail("Could not open media to extract a frame: " +
                                     opened.error().message());
            auto decoder = opened.take();
            auto decoded = decoder->frameAt(*item.timestampUs);
            if (!decoded) return fail("Could not decode the frame at " +
                                      formatTimecode(*item.timestampUs) + ": " +
                                      decoded.error().message());
            const VideoFrameData frame = decoded.take();

            FrameExportService::ExportRequest exportRequest;
            exportRequest.caseId = caseRecord.id;
            exportRequest.caseNumber = caseRecord.caseNumber;
            exportRequest.evidence = *source;
            exportRequest.frame = &frame;
            exportRequest.decoderInfo = decoder->info();
            exportRequest.notes = "Exhibit for report " + outcome.report.title;
            exportRequest.key = key.get();

            auto exported = frames_.exportFrame(exportRequest);
            if (!exported) return fail("Could not export a frame: " + exported.error().message());
            const DerivedAsset asset = exported.take();

            // Decrypted on the way into the bundle: a bundle is checked with
            // sha256sum by somebody who has neither TRACE nor the key.
            auto added = writer.addExhibit(layout_.resolve(asset.storageRelPath), asset.filename,
                                           key.get());
            if (!added) return fail(added.error().message());
            reports_->attachDerivedAsset(item.id, asset.id);

            ExhibitReference reference;
            reference.type = ReportItemType::Frame;
            reference.evidenceNumber = source->evidenceNumber;
            reference.fileName = asset.filename;
            reference.sha256 = asset.sha256.value_or("");
            reference.timestampUs = frame.presentationUs;
            reference.caption = item.caption;
            content.exhibits.push_back(std::move(reference));
            advance("Extracting frames");

        } else if (item.type == ReportItemType::Clip && item.evidenceId && item.rangeStartUs &&
                   item.rangeEndUs) {
            const Evidence* source = evidenceFor(*item.evidenceId);
            if (source == nullptr) return fail("A requested clip refers to evidence not included");

            ClipExportRequest clipRequest;
            clipRequest.caseId = caseRecord.id;
            clipRequest.caseNumber = caseRecord.caseNumber;
            clipRequest.evidence = *source;
            clipRequest.requestedStartUs = *item.rangeStartUs;
            clipRequest.requestedEndUs = *item.rangeEndUs;
            clipRequest.notes = "Exhibit for report " + outcome.report.title;

            auto clipKeyResult = evidence_.caseKey(source->caseId);
            if (!clipKeyResult) return fail(clipKeyResult.error().message());
            // Named rather than chained off take(): the handle owns the key and
            // the pointer must not outlive it.
            const CaseKeyHandle clipKey = clipKeyResult.take();
            clipRequest.key = clipKey.get();

            auto exported = clips_.exportClip(clipRequest);
            if (!exported) return fail("Could not extract a clip: " + exported.error().message());
            const ClipExportOutcome clip = exported.take();

            auto added = writer.addExhibit(layout_.resolve(clip.asset.storageRelPath),
                                           clip.asset.filename, clipKey.get());
            if (!added) return fail(added.error().message());
            reports_->attachDerivedAsset(item.id, clip.asset.id);

            ExhibitReference reference;
            reference.type = ReportItemType::Clip;
            reference.evidenceNumber = source->evidenceNumber;
            reference.fileName = clip.asset.filename;
            reference.sha256 = clip.asset.sha256.value_or("");
            reference.rangeStartUs = clip.requestedStartUs;
            reference.rangeEndUs = clip.requestedEndUs;
            reference.actualStartUs = clip.actualStartUs;
            reference.reencoded = clip.reencoded;
            reference.caption = item.caption;
            content.exhibits.push_back(std::move(reference));
            advance("Extracting clips");
        }
    }

    if (cancelled()) return cancel();

    // ------------------------------------------------------ audit extract
    {
        AuditQuery query;
        query.caseId = caseRecord.id;
        query.limit = 0;  // unlimited: a custody record with pages missing is not one
        query.newestFirst = false;
        auto events = audit_->list(query);
        if (!events) return fail("Could not read the audit trail: " + events.error().message());
        content.auditEvents = events.take();
    }
    advance("Extracting the audit trail");

    // ------------------------------------------------------ write it out
    const std::string html = ReportRenderer::renderHtml(content);
    if (auto added = writer.addTextFile("REPORT.html", html); !added) {
        return fail(added.error().message());
    }
    if (documents_ != nullptr) {
        if (auto status = documents_->renderPdf(html, writer.root() / "REPORT.pdf"); !status) {
            return fail("The paginated report could not be produced: " + status.error().message());
        }
        if (auto added = writer.addProducedFile("REPORT.pdf"); !added) {
            return fail(added.error().message());
        }
    }
    if (auto added = writer.addTextFile("VERIFY.md", BundleVerifier::verifyDocument()); !added) {
        return fail(added.error().message());
    }
    if (auto added = writer.addTextFile("provenance/evidence.json",
                                        ReportRenderer::renderEvidenceJson(content));
        !added) {
        return fail(added.error().message());
    }
    if (auto added = writer.addTextFile("provenance/analysis_runs.json",
                                        ReportRenderer::renderAnalysisRunsJson(content));
        !added) {
        return fail(added.error().message());
    }
    if (auto added = writer.addTextFile("provenance/detections.json",
                                        ReportRenderer::renderDetectionsJson(content));
        !added) {
        return fail(added.error().message());
    }
    if (auto added =
            writer.addTextFile("audit/audit_extract.json", ReportRenderer::renderAuditJson(content));
        !added) {
        return fail(added.error().message());
    }
    if (auto added =
            writer.addTextFile("audit/audit_extract.csv", ReportRenderer::renderAuditCsv(content));
        !added) {
        return fail(added.error().message());
    }
    advance("Writing the report");

    BundleIdentity identity;
    identity.traceVersion = kApplicationVersion;
    identity.exportedAtUtc = exportedAt;
    identity.caseId = caseRecord.id;
    identity.caseNumber = caseRecord.caseNumber;
    identity.caseTitle = caseRecord.title;
    identity.reportId = outcome.report.id;
    identity.reportTitle = outcome.report.title;
    identity.exportedBy = exportedBy;
    identity.includedUnconfirmed = outcome.report.includedUnconfirmed;

    auto manifest = writer.finalise(identity);
    if (!manifest) return fail(manifest.error().message());
    outcome.manifestSha256 = manifest.take();
    outcome.fileCount = static_cast<int>(writer.entries().size());
    advance("Writing the manifest");

    // ------------------------------------------------------- self-check
    // An export that cannot verify its own output is not a success, so this runs the
    // same code a third party will run, before the report is marked exported.
    auto verified = BundleVerifier::verify(outcome.bundlePath);
    if (!verified) return fail("The bundle could not be verified: " + verified.error().message());
    outcome.selfCheck = verified.take();
    if (!outcome.selfCheck.passed()) {
        return fail("The bundle failed its own verification immediately after being written: " +
                    (outcome.selfCheck.problem.empty()
                         ? std::to_string(outcome.selfCheck.failureCount()) + " file(s) did not match"
                         : outcome.selfCheck.problem));
    }
    advance("Verifying the bundle");

    // ------------------------------------------------------- record it
    JsonValue counts = JsonValue::object();
    counts.set("evidence", static_cast<std::int64_t>(content.evidence.size()))
        .set("detections", static_cast<std::int64_t>(content.detections.size()))
        .set("exhibits", static_cast<std::int64_t>(content.exhibits.size()))
        .set("bookmarks", static_cast<std::int64_t>(content.bookmarks.size()))
        .set("annotations", static_cast<std::int64_t>(content.annotations.size()))
        .set("audit_events", static_cast<std::int64_t>(content.auditEvents.size()))
        .set("files", static_cast<std::int64_t>(outcome.fileCount));

    const std::string relative = layout_.relativeTo(outcome.bundlePath);
    if (auto status = reports_->finishExport(
            reportId, ReportStatus::Exported, nowIso8601Utc(),
            relative.empty() ? outcome.bundlePath.string() : relative, outcome.manifestSha256,
            counts.dump(), std::nullopt);
        !status) {
        return fail(status.error().message());
    }

    AuditRecord record;
    record.action = AuditAction::ReportExported;
    record.caseId = caseRecord.id;
    record.caseNumber = caseRecord.caseNumber;
    record.description = "Exhibit bundle exported: " + outcome.report.title;
    record.details = JsonValue::object()
                         .set("report_id", reportId)
                         .set("bundle", outcome.bundlePath.string())
                         .set("manifest_sha256", outcome.manifestSha256)
                         .set("files", static_cast<std::int64_t>(outcome.fileCount))
                         .set("includes_unconfirmed_detections", outcome.report.includedUnconfirmed);
    audit_->record(record);

    logInfo(kComponent, "Exhibit bundle exported",
            JsonValue::object()
                .set("case", caseRecord.caseNumber)
                .set("report", reportId)
                .set("files", static_cast<std::int64_t>(outcome.fileCount)));

    outcome.status = ReportStatus::Exported;
    outcome.report.status = ReportStatus::Exported;
    outcome.report.manifestSha256 = outcome.manifestSha256;
    return ResultType::success(std::move(outcome));
}

Result<BundleVerification> ReportService::verifyBundle(const std::filesystem::path& bundleRoot,
                                                       const std::string& caseId,
                                                       const std::string& caseNumber) {
    auto verified = BundleVerifier::verify(bundleRoot);
    if (!verified) return verified;

    const BundleVerification& report = verified.value();
    AuditRecord record;
    record.action = report.passed() ? AuditAction::BundleVerified
                                    : AuditAction::BundleVerificationFailed;
    if (!caseId.empty()) record.caseId = caseId;
    if (!caseNumber.empty()) record.caseNumber = caseNumber;
    record.outcome = report.passed() ? "success" : "failure";
    record.description = report.passed()
                             ? "Exhibit bundle verified: every file matches its recorded digest"
                             : "Exhibit bundle verification FAILED";
    record.details = JsonValue::object()
                         .set("bundle", bundleRoot.string())
                         .set("manifest_intact", report.manifestIntact)
                         .set("files_checked", static_cast<std::int64_t>(report.files.size()))
                         .set("failures", static_cast<std::int64_t>(report.failureCount()))
                         .set("unlisted_files", static_cast<std::int64_t>(report.unlistedFiles.size()));
    audit_->record(record);

    return verified;
}

Result<std::vector<Report>> ReportService::listForCase(const std::string& caseId) {
    return reports_->listForCase(caseId);
}

Result<std::optional<Report>> ReportService::findReport(const std::string& reportId) {
    return reports_->findReport(reportId);
}

}  // namespace trace
