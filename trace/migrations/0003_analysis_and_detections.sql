-- TRACE schema version 3 — Phase 1 object detection.
--
-- Two tables: the analysis run (what was executed, by which model artefact, over
-- which evidence) and the detections it produced. Provenance for a detection is
-- reached by joining its run: the run names the provider, model, model version
-- and the SHA-256 of the exact model file, so that information is stored once and
-- cannot drift between rows of the same run.
--
-- Detections are relational rows, not a JSON blob: a long recording produces
-- hundreds of thousands of them and they are queried by time, class, confidence
-- and verification state.

CREATE TABLE analysis_runs (
    id                   TEXT PRIMARY KEY NOT NULL,
    case_id              TEXT NOT NULL REFERENCES cases(id) ON DELETE CASCADE ON UPDATE CASCADE,
    evidence_id          TEXT NOT NULL REFERENCES evidence(id) ON DELETE CASCADE ON UPDATE CASCADE,
    analysis_type        TEXT NOT NULL DEFAULT 'object_detection',

    -- What performed the analysis.
    provider_name        TEXT NOT NULL,
    provider_version     TEXT NOT NULL DEFAULT '',
    runtime              TEXT NOT NULL DEFAULT '',
    model_name           TEXT NOT NULL DEFAULT '',
    model_version        TEXT NOT NULL DEFAULT '',
    -- SHA-256 of the model file itself. Two files with the same name are not
    -- necessarily the same artefact; this is what proves which one ran.
    model_sha256         TEXT,
    model_relpath        TEXT,

    device_requested     TEXT NOT NULL DEFAULT 'auto',
    device_used          TEXT NOT NULL DEFAULT '',
    configuration_json   TEXT NOT NULL DEFAULT '{}',

    status               TEXT NOT NULL DEFAULT 'queued',
    started_at           TEXT,
    completed_at         TEXT,

    frames_analyzed      INTEGER NOT NULL DEFAULT 0,
    frames_expected      INTEGER,
    detections_stored    INTEGER NOT NULL DEFAULT 0,
    analyzed_duration_us INTEGER,
    -- Minimum spacing between analysed frames, in microseconds (0 = every
    -- decoded frame). A first-class column rather than a JSON field: the viewer
    -- needs it to decide which analysed frame belongs to the playhead, and
    -- guessing that from the data would put boxes on the wrong frame.
    sampling_interval_us INTEGER,
    -- Source geometry at analysis time, so normalised boxes can be mapped back
    -- to pixels even if the item is later displayed at another size.
    source_width         INTEGER,
    source_height        INTEGER,
    -- The evidence digest as it stood when the run started: an analysis is only
    -- meaningful against a known state of the original.
    evidence_sha256      TEXT NOT NULL DEFAULT '',

    error_message        TEXT,
    warnings_json        TEXT NOT NULL DEFAULT '[]',
    created_by           TEXT NOT NULL DEFAULT '',
    created_at           TEXT NOT NULL
);

CREATE INDEX idx_analysis_runs_case ON analysis_runs(case_id);
CREATE INDEX idx_analysis_runs_evidence ON analysis_runs(evidence_id, created_at DESC);
CREATE INDEX idx_analysis_runs_status ON analysis_runs(status);
CREATE INDEX idx_analysis_runs_created ON analysis_runs(created_at DESC);

CREATE TABLE detections (
    id                TEXT PRIMARY KEY NOT NULL,
    analysis_run_id   TEXT NOT NULL REFERENCES analysis_runs(id) ON DELETE CASCADE ON UPDATE CASCADE,
    case_id           TEXT NOT NULL REFERENCES cases(id) ON DELETE CASCADE ON UPDATE CASCADE,
    evidence_id       TEXT NOT NULL REFERENCES evidence(id) ON DELETE CASCADE ON UPDATE CASCADE,

    -- Position in the media. The presentation timestamp is authoritative;
    -- frame_number is recorded only when the stream's timing makes an index
    -- meaningful (see the Phase 0 rule on variable frame rate).
    timestamp_us      INTEGER NOT NULL,
    frame_number      INTEGER,

    class_id          INTEGER NOT NULL,
    class_label       TEXT NOT NULL,
    -- Coarse grouping used by the timeline lanes and filters: person | vehicle |
    -- object. Stored per row so results stay readable if a later run uses a model
    -- with a different class list.
    class_group       TEXT NOT NULL DEFAULT 'object',
    confidence        REAL NOT NULL,

    -- Normalised box, origin top-left, fractions of the source frame (0..1).
    -- This is the authoritative geometry and is resolution independent.
    bbox_x            REAL NOT NULL,
    bbox_y            REAL NOT NULL,
    bbox_w            REAL NOT NULL,
    bbox_h            REAL NOT NULL,
    -- Convenience copy in source pixels at analysis-time resolution.
    bbox_pixel_x      INTEGER,
    bbox_pixel_y      INTEGER,
    bbox_pixel_w      INTEGER,
    bbox_pixel_h      INTEGER,

    -- Human review. Machine output is never self-certified.
    verification_state TEXT NOT NULL DEFAULT 'unreviewed',
    verified_by        TEXT,
    verified_at        TEXT,
    analyst_note       TEXT NOT NULL DEFAULT '',

    created_at         TEXT NOT NULL
);

-- Playback and overlay query by (evidence, time); the panel filters by class,
-- confidence and review state.
CREATE INDEX idx_detections_evidence_time ON detections(evidence_id, timestamp_us);
CREATE INDEX idx_detections_run ON detections(analysis_run_id);
CREATE INDEX idx_detections_case ON detections(case_id);
CREATE INDEX idx_detections_class ON detections(evidence_id, class_group, timestamp_us);
CREATE INDEX idx_detections_confidence ON detections(evidence_id, confidence);
CREATE INDEX idx_detections_verification ON detections(evidence_id, verification_state);
