#include "core/reporting/report_renderer.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "core/common/json.h"
#include "core/common/time_utils.h"

namespace trace {
namespace {

/// HTML-escapes text that came from a case file, a filename or an analyst's note.
/// Everything user-supplied goes through this — a bundle must render the same way
/// wherever it is opened, and an apostrophe in a case title is not markup.
std::string esc(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string timecode(std::int64_t us) {
    if (us < 0) us = 0;
    const std::int64_t totalMs = us / 1000;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld.%03lld",
                  static_cast<long long>(totalMs / 3600000),
                  static_cast<long long>((totalMs / 60000) % 60),
                  static_cast<long long>((totalMs / 1000) % 60),
                  static_cast<long long>(totalMs % 1000));
    return buffer;
}

std::string percent(double value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%.1f%%", value * 100.0);
    return buffer;
}

std::string orNotAvailable(const std::optional<std::string>& value) {
    return value && !value->empty() ? esc(*value) : "<em>Not available</em>";
}

std::string orNotAvailable(const std::optional<std::int64_t>& value) {
    return value ? std::to_string(*value) : "<em>Not available</em>";
}

void jsonSetOptional(JsonValue& object, const std::string& key,
                     const std::optional<std::string>& value) {
    if (value) object.set(key, *value);
}

void jsonSetOptional(JsonValue& object, const std::string& key,
                     const std::optional<std::int64_t>& value) {
    if (value) object.set(key, *value);
}

}  // namespace

std::string ReportRenderer::renderHtml(const ReportContent& content) {
    std::ostringstream h;
    const Case& c = content.caseRecord;

    h << "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
      << "<title>TRACE observation report — " << esc(c.caseNumber) << "</title>\n"
      << "<style>\n"
      << ":root{color-scheme:light}\n"
      << "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;line-height:1.55;"
         "max-width:60rem;margin:0 auto;padding:2rem 1.5rem;color:#16191c;background:#fff}\n"
      << "h1{font-size:1.6rem;margin:0 0 .25rem}h2{font-size:1.15rem;margin:2.25rem 0 .5rem;"
         "border-bottom:1px solid #d7dbe0;padding-bottom:.3rem}\n"
      << "table{border-collapse:collapse;width:100%;margin:.75rem 0;font-size:.9rem}\n"
      << "th,td{border:1px solid #d7dbe0;padding:.4rem .55rem;text-align:left;vertical-align:top}\n"
      << "th{background:#f3f5f7;font-weight:600}\n"
      << ".mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:.8rem;"
         "word-break:break-all}\n"
      << ".limits{border-left:3px solid #16191c;background:#f7f8f9;padding:.9rem 1.1rem;"
         "margin:1.25rem 0}\n"
      << ".warn{border-left:3px solid #a4630f;background:#fdf6ec;padding:.9rem 1.1rem;"
         "margin:1.25rem 0}\n"
      << "figure{margin:1.25rem 0;border:1px solid #d7dbe0;padding:.6rem}\n"
      << "figure img{max-width:100%;height:auto;display:block}\n"
      << "figcaption{font-size:.85rem;color:#4a5158;margin-top:.5rem}\n"
      << "em{color:#6b7278}\n"
      << "</style></head><body>\n";

    // ------------------------------------------------------------- header
    h << "<h1>TRACE observation report</h1>\n"
      << "<p class=\"mono\">" << esc(content.report.title) << "</p>\n"
      << "<table>\n"
      << "<tr><th>Case</th><td>" << esc(c.caseNumber) << " — " << esc(c.title) << "</td></tr>\n"
      << "<tr><th>Investigator</th><td>" << esc(c.investigator) << "</td></tr>\n";
    if (!c.agency.empty()) h << "<tr><th>Agency</th><td>" << esc(c.agency) << "</td></tr>\n";
    h << "<tr><th>Exported</th><td>" << esc(content.exportedAtUtc) << " by "
      << esc(content.exportedBy) << "</td></tr>\n"
      << "<tr><th>Software</th><td>TRACE " << esc(content.traceVersion) << "</td></tr>\n"
      << "<tr><th>Report id</th><td class=\"mono\">" << esc(content.report.id) << "</td></tr>\n"
      << "</table>\n";

    // -------------------------------------------------------- limitations
    h << "<h2>Limitations</h2>\n<div class=\"limits\">"
      << esc(BundleWriter::limitationsStatement()) << "</div>\n";

    if (content.report.includedUnconfirmed) {
        h << "<div class=\"warn\"><strong>This report includes detections that no analyst has "
             "confirmed.</strong> Their review state is shown against each one below. An "
             "unreviewed or rejected detection is a machine observation that has not been "
             "agreed with by a person.</div>\n";
    }

    // ----------------------------------------------------------- evidence
    h << "<h2>Evidence</h2>\n<table>\n"
      << "<tr><th>Item</th><th>Original filename</th><th>Ingested (UTC)</th><th>Size</th>"
         "<th>Integrity</th></tr>\n";
    for (const auto& e : content.evidence) {
        h << "<tr><td>" << esc(e.evidenceNumber) << "</td><td>" << esc(e.originalFilename)
          << "</td><td>" << esc(e.ingestedAt) << "</td><td>" << e.fileSize << " bytes</td><td>"
          << esc(toDisplayString(e.integrityStatus));
        if (e.lastVerifiedAt) h << "<br><small>" << esc(*e.lastVerifiedAt) << "</small>";
        h << "</td></tr>\n";
    }
    h << "</table>\n";

    h << "<table>\n<tr><th>Item</th><th>SHA-256 of the managed original</th></tr>\n";
    for (const auto& e : content.evidence) {
        h << "<tr><td>" << esc(e.evidenceNumber) << "</td><td class=\"mono\">" << esc(e.sha256)
          << "</td></tr>\n";
    }
    h << "</table>\n";

    // Technical detail, where the container actually provided it.
    h << "<h2>Technical detail</h2>\n<table>\n"
      << "<tr><th>Item</th><th>Container</th><th>Video codec</th><th>Resolution</th>"
         "<th>Duration</th><th>Frame rate</th></tr>\n";
    for (std::size_t i = 0; i < content.evidence.size(); ++i) {
        const auto& e = content.evidence[i];
        const auto& m = i < content.evidenceMetadata.size() ? content.evidenceMetadata[i]
                                                            : std::optional<MediaMetadata>{};
        h << "<tr><td>" << esc(e.evidenceNumber) << "</td>";
        if (!m) {
            h << "<td colspan=\"5\"><em>Not available</em></td></tr>\n";
            continue;
        }
        h << "<td>" << orNotAvailable(m->containerFormat) << "</td>"
          << "<td>" << orNotAvailable(m->videoCodec) << "</td><td>";
        if (m->width && m->height) h << *m->width << " × " << *m->height;
        else h << "<em>Not available</em>";
        h << "</td><td>";
        if (m->durationUs) h << esc(timecode(*m->durationUs));
        else h << "<em>Not available</em>";
        h << "</td><td>";
        if (m->averageFrameRate) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.3f fps", *m->averageFrameRate);
            h << buffer;
        } else {
            h << "<em>Not available</em>";
        }
        h << "</td></tr>\n";
    }
    h << "</table>\n";

    // ----------------------------------------------------------- findings
    h << "<h2>Observations</h2>\n";
    if (content.detections.empty()) {
        h << "<p><em>No detections were included in this report.</em></p>\n";
    } else {
        h << "<table>\n<tr><th>Item</th><th>Time</th><th>Class</th><th>Confidence</th>"
             "<th>Review</th><th>Box (normalised)</th></tr>\n";
        for (const auto& cited : content.detections) {
            const Detection& d = cited.detection;
            char box[96];
            std::snprintf(box, sizeof(box), "x %.4f  y %.4f  w %.4f  h %.4f", d.box.x, d.box.y,
                          d.box.width, d.box.height);
            h << "<tr><td>" << esc(cited.evidenceNumber) << "</td><td class=\"mono\">"
              << esc(timecode(d.timestampUs)) << "</td><td>" << esc(d.classLabel) << "</td><td>"
              << esc(percent(d.confidence)) << "</td><td>"
              << esc(toDisplayString(d.verification));
            if (d.verifiedBy) h << "<br><small>" << esc(*d.verifiedBy);
            if (d.verifiedAt) h << "<br>" << esc(*d.verifiedAt);
            if (d.verifiedBy) h << "</small>";
            h << "</td><td class=\"mono\">" << esc(box) << "</td></tr>\n";
        }
        h << "</table>\n";
        h << "<p>A confidence score is the model's own score for a visual class. It is not a "
             "probability that the object is what the label says.</p>\n";
    }

    // ---------------------------------------------------------- exhibits
    if (!content.exhibits.empty()) {
        h << "<h2>Exhibits</h2>\n";
        for (const auto& x : content.exhibits) {
            h << "<figure>\n";
            if (x.type == ReportItemType::Frame) {
                h << "<img src=\"exhibits/" << esc(x.fileName) << "\" alt=\""
                  << esc(x.evidenceNumber) << " at " << esc(timecode(x.timestampUs)) << "\">\n";
            }
            h << "<figcaption><strong>" << esc(x.fileName) << "</strong><br>"
              << esc(x.evidenceNumber) << " · ";
            if (x.type == ReportItemType::Clip) {
                h << "clip " << esc(timecode(x.rangeStartUs.value_or(0))) << " – "
                  << esc(timecode(x.rangeEndUs.value_or(0)));
                if (x.actualStartUs && *x.actualStartUs != x.rangeStartUs.value_or(0)) {
                    h << "<br>Extraction began at " << esc(timecode(*x.actualStartUs))
                      << ", the nearest preceding keyframe: a stream copy cannot begin "
                         "mid-frame.";
                }
                h << "<br>" << (x.reencoded ? "Re-encoded" : "Stream copy — the original "
                                                             "encoded frames, not re-encoded");
            } else {
                h << "frame at " << esc(timecode(x.timestampUs));
            }
            if (!x.caption.empty()) h << "<br>" << esc(x.caption);
            h << "<br><span class=\"mono\">SHA-256 " << esc(x.sha256) << "</span>"
              << "</figcaption>\n</figure>\n";
        }
    }

    // ------------------------------------------------- analysis provenance
    h << "<h2>Analysis provenance</h2>\n";
    std::vector<std::string> seenRuns;
    bool anyRun = false;
    for (const auto& cited : content.detections) {
        if (std::find(seenRuns.begin(), seenRuns.end(), cited.run.id) != seenRuns.end()) continue;
        seenRuns.push_back(cited.run.id);
        const AnalysisRun& r = cited.run;
        if (!anyRun) {
            h << "<table>\n<tr><th>Field</th><th>Value</th></tr>\n";
            anyRun = true;
        }
        h << "<tr><th colspan=\"2\">Run " << esc(r.id) << "</th></tr>\n"
          << "<tr><td>Provider</td><td>" << esc(r.providerName) << " " << esc(r.providerVersion)
          << "</td></tr>\n"
          << "<tr><td>Runtime</td><td>" << esc(r.runtime) << "</td></tr>\n"
          << "<tr><td>Model</td><td>" << esc(r.modelName) << " " << esc(r.modelVersion)
          << "</td></tr>\n"
          << "<tr><td>Model SHA-256</td><td class=\"mono\">"
          << (r.modelSha256 ? esc(*r.modelSha256)
                            : std::string("<em>Not applicable — this provider runs no model "
                                          "artefact</em>"))
          << "</td></tr>\n"
          << "<tr><td>Device used</td><td>" << esc(r.deviceUsed) << " (requested "
          << esc(r.deviceRequested) << ")</td></tr>\n"
          << "<tr><td>Sampling</td><td>";
        if (r.samplingIntervalUs && *r.samplingIntervalUs > 0) {
            h << "at most one analysed frame every " << (*r.samplingIntervalUs / 1000) << " ms";
        } else {
            h << "every decoded frame";
        }
        h << "</td></tr>\n"
          << "<tr><td>Configuration</td><td class=\"mono\">" << esc(r.configurationJson)
          << "</td></tr>\n"
          << "<tr><td>Evidence SHA-256 at run time</td><td class=\"mono\">"
          << esc(r.evidenceSha256) << "</td></tr>\n"
          << "<tr><td>Frames analysed</td><td>" << r.framesAnalyzed << "</td></tr>\n"
          << "<tr><td>Run status</td><td>" << esc(toDisplayString(r.status));
        if (!producedCompleteResults(r.status)) {
            h << " — these results do not cover the whole recording";
        }
        h << "</td></tr>\n";
    }
    if (anyRun) h << "</table>\n";
    else h << "<p><em>No analysis results are cited in this report.</em></p>\n";

    // -------------------------------------------- bookmarks and notes
    if (!content.bookmarks.empty()) {
        h << "<h2>Bookmarks</h2>\n<table>\n<tr><th>Time</th><th>Title</th><th>Note</th>"
             "<th>Recorded by</th></tr>\n";
        for (const auto& b : content.bookmarks) {
            h << "<tr><td class=\"mono\">" << esc(timecode(b.timestampUs)) << "</td><td>"
              << esc(b.title) << "</td><td>" << esc(b.note) << "</td><td>" << esc(b.createdBy)
              << "</td></tr>\n";
        }
        h << "</table>\n";
    }
    if (!content.annotations.empty()) {
        h << "<h2>Notes</h2>\n<table>\n<tr><th>Time</th><th>Note</th><th>Recorded by</th></tr>\n";
        for (const auto& a : content.annotations) {
            h << "<tr><td class=\"mono\">" << esc(timecode(a.startUs));
            if (a.endUs) h << " – " << esc(timecode(*a.endUs));
            h << "</td><td>" << esc(a.text) << "</td><td>" << esc(a.createdBy) << "</td></tr>\n";
        }
        h << "</table>\n";
    }

    // --------------------------------------------------- chain of custody
    h << "<h2>Chain of custody</h2>\n"
      << "<p>Every action TRACE recorded against this material, oldest first. The audit trail "
         "is append-only and enforced by the database itself.</p>\n"
      << "<table>\n<tr><th>#</th><th>When (UTC)</th><th>Action</th><th>Actor</th>"
         "<th>Description</th><th>Outcome</th></tr>\n";
    for (const auto& e : content.auditEvents) {
        h << "<tr><td>" << e.sequence << "</td><td class=\"mono\">" << esc(e.occurredAt)
          << "</td><td>" << esc(toString(e.action)) << "</td><td>" << esc(e.actor) << "</td><td>"
          << esc(e.description) << "</td><td>" << esc(e.outcome) << "</td></tr>\n";
    }
    h << "</table>\n";

    h << "<h2>Verifying this bundle</h2>\n"
      << "<p>Every file in this bundle is listed with the SHA-256 of the bytes as written. See "
         "<code>VERIFY.md</code> beside this report for the commands. Nothing here is digitally "
         "signed: these are integrity digests, not signatures.</p>\n";

    h << "</body></html>\n";
    return h.str();
}

std::string ReportRenderer::renderEvidenceJson(const ReportContent& content) {
    JsonValue root = JsonValue::object();
    root.set("case_number", content.caseRecord.caseNumber)
        .set("exported_at_utc", content.exportedAtUtc);

    JsonValue items = JsonValue::array();
    for (std::size_t i = 0; i < content.evidence.size(); ++i) {
        const auto& e = content.evidence[i];
        JsonValue entry = JsonValue::object();
        entry.set("evidence_id", e.id)
            .set("evidence_number", e.evidenceNumber)
            .set("original_filename", e.originalFilename)
            .set("stored_filename", e.storedFilename)
            .set("file_size_bytes", e.fileSize)
            .set("sha256", e.sha256)
            .set("ingested_at", e.ingestedAt)
            .set("ingested_by", e.ingestedBy)
            .set("integrity_status", toString(e.integrityStatus))
            .set("media_status", toString(e.mediaStatus));
        jsonSetOptional(entry, "last_verified_at", e.lastVerifiedAt);
        jsonSetOptional(entry, "mime_type", e.mimeType);

        if (i < content.evidenceMetadata.size() && content.evidenceMetadata[i]) {
            const auto& m = *content.evidenceMetadata[i];
            JsonValue media = JsonValue::object();
            jsonSetOptional(media, "container", m.containerFormat);
            jsonSetOptional(media, "video_codec", m.videoCodec);
            jsonSetOptional(media, "width", m.width);
            jsonSetOptional(media, "height", m.height);
            jsonSetOptional(media, "duration_us", m.durationUs);
            if (m.averageFrameRate) media.set("average_frame_rate", *m.averageFrameRate);
            entry.set("media", media);
        }
        items.push(entry);
    }
    root.set("evidence", items);
    return root.dump(2) + "\n";
}

std::string ReportRenderer::renderAnalysisRunsJson(const ReportContent& content) {
    JsonValue root = JsonValue::object();
    root.set("exported_at_utc", content.exportedAtUtc);

    JsonValue runs = JsonValue::array();
    std::vector<std::string> seen;
    for (const auto& cited : content.detections) {
        if (std::find(seen.begin(), seen.end(), cited.run.id) != seen.end()) continue;
        seen.push_back(cited.run.id);
        const AnalysisRun& r = cited.run;

        JsonValue entry = JsonValue::object();
        entry.set("run_id", r.id)
            .set("evidence_id", r.evidenceId)
            .set("analysis_type", r.analysisType)
            .set("provider_name", r.providerName)
            .set("provider_version", r.providerVersion)
            .set("runtime", r.runtime)
            .set("model_name", r.modelName)
            .set("model_version", r.modelVersion)
            .set("device_requested", r.deviceRequested)
            .set("device_used", r.deviceUsed)
            .set("configuration", r.configurationJson)
            .set("status", toString(r.status))
            .set("produced_complete_results", producedCompleteResults(r.status))
            .set("frames_analyzed", r.framesAnalyzed)
            .set("detections_stored", r.detectionsStored)
            .set("evidence_sha256_at_run_time", r.evidenceSha256);
        jsonSetOptional(entry, "model_sha256", r.modelSha256);
        jsonSetOptional(entry, "model_relpath", r.modelRelPath);
        jsonSetOptional(entry, "sampling_interval_us", r.samplingIntervalUs);
        jsonSetOptional(entry, "started_at", r.startedAt);
        jsonSetOptional(entry, "completed_at", r.completedAt);
        jsonSetOptional(entry, "error_message", r.errorMessage);
        runs.push(entry);
    }
    root.set("analysis_runs", runs);
    return root.dump(2) + "\n";
}

std::string ReportRenderer::renderDetectionsJson(const ReportContent& content) {
    JsonValue root = JsonValue::object();
    root.set("exported_at_utc", content.exportedAtUtc)
        .set("includes_unconfirmed", content.report.includedUnconfirmed);

    JsonValue items = JsonValue::array();
    for (const auto& cited : content.detections) {
        const Detection& d = cited.detection;
        JsonValue box = JsonValue::object();
        box.set("x", d.box.x).set("y", d.box.y).set("width", d.box.width).set("height", d.box.height);

        JsonValue entry = JsonValue::object();
        entry.set("detection_id", d.id)
            .set("analysis_run_id", d.analysisRunId)
            .set("evidence_id", d.evidenceId)
            .set("evidence_number", cited.evidenceNumber)
            .set("timestamp_us", d.timestampUs)
            .set("class_label", d.classLabel)
            .set("class_group", toString(d.classGroup))
            .set("confidence", d.confidence)
            .set("box_normalised", box)
            .set("verification_state", toString(d.verification))
            .set("analyst_note", d.analystNote);
        jsonSetOptional(entry, "frame_number", d.frameNumber);
        jsonSetOptional(entry, "verified_by", d.verifiedBy);
        jsonSetOptional(entry, "verified_at", d.verifiedAt);
        items.push(entry);
    }
    root.set("detections", items);
    return root.dump(2) + "\n";
}

std::string ReportRenderer::renderAuditJson(const ReportContent& content) {
    JsonValue root = JsonValue::object();
    root.set("case_number", content.caseRecord.caseNumber)
        .set("exported_at_utc", content.exportedAtUtc)
        .set("event_count", static_cast<std::int64_t>(content.auditEvents.size()));

    JsonValue events = JsonValue::array();
    for (const auto& e : content.auditEvents) {
        JsonValue entry = JsonValue::object();
        entry.set("sequence", e.sequence)
            .set("occurred_at", e.occurredAt)
            .set("action", toString(e.action))
            .set("actor", e.actor)
            .set("description", e.description)
            .set("details", e.detailsJson)
            .set("outcome", e.outcome)
            .set("app_version", e.appVersion);
        jsonSetOptional(entry, "case_number", e.caseNumber);
        jsonSetOptional(entry, "evidence_number", e.evidenceNumber);
        events.push(entry);
    }
    root.set("events", events);
    return root.dump(2) + "\n";
}

std::string ReportRenderer::renderAuditCsv(const ReportContent& content) {
    const auto quote = [](const std::string& value) {
        std::string out = "\"";
        for (char c : value) {
            if (c == '"') out += "\"\"";
            else if (c == '\n' || c == '\r') out += ' ';
            else out += c;
        }
        return out + "\"";
    };

    std::ostringstream csv;
    csv << "sequence,occurred_at_utc,action,actor,case_number,evidence_number,description,outcome\n";
    for (const auto& e : content.auditEvents) {
        csv << e.sequence << ',' << quote(e.occurredAt) << ',' << quote(toString(e.action)) << ','
            << quote(e.actor) << ',' << quote(e.caseNumber.value_or("")) << ','
            << quote(e.evidenceNumber.value_or("")) << ',' << quote(e.description) << ','
            << quote(e.outcome) << '\n';
    }
    return csv.str();
}

}  // namespace trace
