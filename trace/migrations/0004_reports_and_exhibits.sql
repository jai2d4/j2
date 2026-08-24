-- TRACE schema version 4 — Phase 2 reports and exhibit export.
--
-- A report is a selection: which evidence, which findings, which frames and clips an
-- analyst chose to put in front of someone else. Exporting it writes a self-contained
-- bundle whose every file carries a SHA-256, so a third party can verify the material
-- without TRACE and without trusting it.
--
-- The bundle is not a special case in the provenance model: every file it contains is
-- also a derived_assets row with a processing_operations parent, exactly like a frame
-- export. This table records the selection and the outcome of writing it.

CREATE TABLE reports (
    id                   TEXT PRIMARY KEY NOT NULL,
    case_id              TEXT NOT NULL REFERENCES cases(id) ON DELETE CASCADE ON UPDATE CASCADE,
    title                TEXT NOT NULL,

    -- Lifecycle. A report that failed to export is never recorded as exported; the
    -- repository refuses the combination, the same way an analysis run does.
    status               TEXT NOT NULL DEFAULT 'draft',

    -- Where the bundle was written, relative to the data root, and the digest of its
    -- MANIFEST.json. Together these are enough to re-verify an export later.
    bundle_relpath       TEXT,
    manifest_sha256      TEXT,

    -- The software that produced it, so a bundle can always name its own origin.
    trace_version        TEXT NOT NULL DEFAULT '',

    export_started_at    TEXT,
    export_completed_at  TEXT,
    -- How many of each item type were written, as JSON, for display without a join.
    item_counts_json     TEXT NOT NULL DEFAULT '{}',
    -- True when the operator deliberately included detections a human had not
    -- confirmed. Stored rather than inferred: it changes how the report reads.
    included_unconfirmed INTEGER NOT NULL DEFAULT 0,

    error_message        TEXT,
    created_by           TEXT NOT NULL DEFAULT '',
    created_at           TEXT NOT NULL
);

-- One row per thing the operator chose to include.
CREATE TABLE report_items (
    id                TEXT PRIMARY KEY NOT NULL,
    report_id         TEXT NOT NULL REFERENCES reports(id) ON DELETE CASCADE ON UPDATE CASCADE,
    item_type         TEXT NOT NULL,

    -- Which record this item points at. Exactly which of these is set depends on
    -- item_type; a frame or clip also carries the derived asset once it is produced.
    evidence_id       TEXT REFERENCES evidence(id) ON DELETE CASCADE ON UPDATE CASCADE,
    detection_id      TEXT,
    bookmark_id       TEXT,
    annotation_id     TEXT,
    derived_asset_id  TEXT,

    -- For frames, the moment. For clips, the requested range.
    timestamp_us      INTEGER,
    range_start_us    INTEGER,
    range_end_us      INTEGER,

    caption           TEXT NOT NULL DEFAULT '',
    sort_order        INTEGER NOT NULL DEFAULT 0,
    created_at        TEXT NOT NULL
);

CREATE INDEX idx_reports_case ON reports(case_id, created_at DESC);
CREATE INDEX idx_reports_status ON reports(status);
CREATE INDEX idx_report_items_report ON report_items(report_id, sort_order);
CREATE INDEX idx_report_items_evidence ON report_items(evidence_id);
CREATE INDEX idx_report_items_type ON report_items(report_id, item_type);
