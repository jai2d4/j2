-- TRACE initial schema (version 1).
--
-- Design rules encoded here:
--   * every entity has an opaque UUID primary key; display numbers such as
--     CASE-0001 / EVD-000001 are labels, never identity;
--   * case-scoped content cascades on case deletion, but audit rows do not
--     reference cases with a foreign key at all, so the audit trail survives
--     any deletion (identifiers are denormalised into the row);
--   * media positions are stored as integer microseconds, never as frame
--     indexes, so variable-frame-rate evidence stays exact;
--   * "unknown" is NULL, never a zero or an invented default.

CREATE TABLE cases (
    id             TEXT PRIMARY KEY NOT NULL,
    case_number    TEXT NOT NULL UNIQUE,
    title          TEXT NOT NULL,
    description    TEXT NOT NULL DEFAULT '',
    status         TEXT NOT NULL DEFAULT 'open',
    investigator   TEXT NOT NULL DEFAULT '',
    agency         TEXT NOT NULL DEFAULT '',
    location       TEXT NOT NULL DEFAULT '',
    notes          TEXT NOT NULL DEFAULT '',
    created_at     TEXT NOT NULL,
    modified_at    TEXT NOT NULL,
    created_by     TEXT NOT NULL DEFAULT '',
    storage_relpath TEXT NOT NULL
);

CREATE INDEX idx_cases_status ON cases(status);
CREATE INDEX idx_cases_modified ON cases(modified_at DESC);

CREATE TABLE evidence (
    id                 TEXT PRIMARY KEY NOT NULL,
    case_id            TEXT NOT NULL REFERENCES cases(id) ON DELETE CASCADE ON UPDATE CASCADE,
    evidence_number    TEXT NOT NULL,
    original_filename  TEXT NOT NULL,
    stored_filename    TEXT NOT NULL,
    source_path        TEXT NOT NULL,
    storage_relpath    TEXT NOT NULL,
    media_type         TEXT NOT NULL DEFAULT 'unknown',
    mime_type          TEXT,
    file_size          INTEGER NOT NULL,
    sha256             TEXT NOT NULL,
    source_modified_at TEXT,
    ingested_at        TEXT NOT NULL,
    modified_at        TEXT NOT NULL,
    ingested_by        TEXT NOT NULL DEFAULT '',
    integrity_status   TEXT NOT NULL DEFAULT 'not_verified',
    last_verified_at   TEXT,
    media_status       TEXT NOT NULL DEFAULT 'unknown',
    description        TEXT NOT NULL DEFAULT '',
    UNIQUE(case_id, evidence_number)
);

CREATE INDEX idx_evidence_case ON evidence(case_id);
CREATE INDEX idx_evidence_sha256 ON evidence(sha256);
CREATE INDEX idx_evidence_ingested ON evidence(ingested_at DESC);

-- One technical-metadata row per evidence item. Columns hold the fields TRACE
-- displays and queries; raw_metadata_json preserves everything the container
-- reported, verbatim, for later review.
CREATE TABLE evidence_metadata (
    evidence_id        TEXT PRIMARY KEY NOT NULL
                       REFERENCES evidence(id) ON DELETE CASCADE ON UPDATE CASCADE,
    container_format   TEXT,
    format_long_name   TEXT,
    duration_us        INTEGER,
    bit_rate           INTEGER,
    video_codec        TEXT,
    video_codec_long   TEXT,
    video_profile      TEXT,
    width              INTEGER,
    height             INTEGER,
    pixel_format       TEXT,
    display_aspect     TEXT,
    avg_frame_rate     REAL,
    nominal_frame_rate REAL,
    frame_rate_mode    TEXT,
    video_time_base    TEXT,
    video_bit_rate     INTEGER,
    frame_count        INTEGER,
    audio_codec        TEXT,
    audio_codec_long   TEXT,
    audio_channels     INTEGER,
    audio_channel_layout TEXT,
    audio_sample_rate  INTEGER,
    audio_sample_format TEXT,
    audio_bit_rate     INTEGER,
    video_stream_count INTEGER NOT NULL DEFAULT 0,
    audio_stream_count INTEGER NOT NULL DEFAULT 0,
    subtitle_stream_count INTEGER NOT NULL DEFAULT 0,
    creation_time      TEXT,
    gps_location       TEXT,
    make               TEXT,
    model              TEXT,
    raw_metadata_json  TEXT NOT NULL DEFAULT '{}',
    extraction_status  TEXT NOT NULL DEFAULT 'unknown',
    extraction_error   TEXT,
    extractor          TEXT NOT NULL DEFAULT '',
    extracted_at       TEXT NOT NULL
);

-- Per-stream detail, relational rather than buried in JSON, because future
-- modules (audio analysis, multi-camera sync) select on it.
CREATE TABLE media_streams (
    id            TEXT PRIMARY KEY NOT NULL,
    evidence_id   TEXT NOT NULL REFERENCES evidence(id) ON DELETE CASCADE ON UPDATE CASCADE,
    stream_index  INTEGER NOT NULL,
    stream_type   TEXT NOT NULL,
    codec_name    TEXT,
    codec_long_name TEXT,
    time_base     TEXT,
    duration_us   INTEGER,
    bit_rate      INTEGER,
    width         INTEGER,
    height        INTEGER,
    pixel_format  TEXT,
    frame_rate    REAL,
    sample_rate   INTEGER,
    channels      INTEGER,
    channel_layout TEXT,
    language      TEXT,
    metadata_json TEXT NOT NULL DEFAULT '{}',
    UNIQUE(evidence_id, stream_index)
);

CREATE INDEX idx_media_streams_evidence ON media_streams(evidence_id);

-- Provenance: the operation that produced a derived asset, including the exact
-- software/model and parameters used. Analysis results in later phases record
-- themselves here too, which is what makes every future conclusion traceable.
CREATE TABLE processing_operations (
    id                  TEXT PRIMARY KEY NOT NULL,
    case_id             TEXT NOT NULL REFERENCES cases(id) ON DELETE CASCADE ON UPDATE CASCADE,
    evidence_id         TEXT NOT NULL REFERENCES evidence(id) ON DELETE CASCADE ON UPDATE CASCADE,
    operation_type      TEXT NOT NULL,
    parameters_json     TEXT NOT NULL DEFAULT '{}',
    software_name       TEXT NOT NULL,
    software_version    TEXT NOT NULL,
    library_versions    TEXT NOT NULL DEFAULT '{}',
    model_name          TEXT,
    model_version       TEXT,
    source_start_us     INTEGER,
    source_end_us       INTEGER,
    source_frame_number INTEGER,
    started_at          TEXT NOT NULL,
    completed_at        TEXT,
    status              TEXT NOT NULL DEFAULT 'completed',
    performed_by        TEXT NOT NULL DEFAULT '',
    error_message       TEXT,
    notes               TEXT NOT NULL DEFAULT ''
);

CREATE INDEX idx_operations_evidence ON processing_operations(evidence_id);
CREATE INDEX idx_operations_type ON processing_operations(operation_type);

CREATE TABLE derived_assets (
    id                  TEXT PRIMARY KEY NOT NULL,
    case_id             TEXT NOT NULL REFERENCES cases(id) ON DELETE CASCADE ON UPDATE CASCADE,
    evidence_id         TEXT NOT NULL REFERENCES evidence(id) ON DELETE CASCADE ON UPDATE CASCADE,
    operation_id        TEXT REFERENCES processing_operations(id) ON DELETE SET NULL ON UPDATE CASCADE,
    parent_asset_id     TEXT REFERENCES derived_assets(id) ON DELETE SET NULL ON UPDATE CASCADE,
    asset_type          TEXT NOT NULL,
    storage_relpath     TEXT NOT NULL,
    filename            TEXT NOT NULL,
    media_type          TEXT,
    file_size           INTEGER,
    sha256              TEXT,
    source_start_us     INTEGER,
    source_end_us       INTEGER,
    source_frame_number INTEGER,
    created_at          TEXT NOT NULL,
    created_by          TEXT NOT NULL DEFAULT '',
    verification_state  TEXT NOT NULL DEFAULT 'unverified',
    verified_by         TEXT,
    verified_at         TEXT,
    notes               TEXT NOT NULL DEFAULT ''
);

CREATE INDEX idx_derived_evidence ON derived_assets(evidence_id);
CREATE INDEX idx_derived_case ON derived_assets(case_id);
CREATE INDEX idx_derived_type ON derived_assets(asset_type);

CREATE TABLE bookmarks (
    id           TEXT PRIMARY KEY NOT NULL,
    case_id      TEXT NOT NULL REFERENCES cases(id) ON DELETE CASCADE ON UPDATE CASCADE,
    evidence_id  TEXT NOT NULL REFERENCES evidence(id) ON DELETE CASCADE ON UPDATE CASCADE,
    timestamp_us INTEGER NOT NULL,
    frame_number INTEGER,
    title        TEXT NOT NULL,
    note         TEXT NOT NULL DEFAULT '',
    colour       TEXT,
    created_by   TEXT NOT NULL DEFAULT '',
    created_at   TEXT NOT NULL,
    modified_at  TEXT NOT NULL
);

CREATE INDEX idx_bookmarks_evidence_time ON bookmarks(evidence_id, timestamp_us);
CREATE INDEX idx_bookmarks_case ON bookmarks(case_id);

-- annotation_type carries the Phase 0 kinds (timestamp_note, range_note) and
-- leaves room for point/region/measurement geometry, which lands in
-- geometry_json without a schema change.
CREATE TABLE annotations (
    id              TEXT PRIMARY KEY NOT NULL,
    case_id         TEXT NOT NULL REFERENCES cases(id) ON DELETE CASCADE ON UPDATE CASCADE,
    evidence_id     TEXT NOT NULL REFERENCES evidence(id) ON DELETE CASCADE ON UPDATE CASCADE,
    annotation_type TEXT NOT NULL DEFAULT 'timestamp_note',
    start_us        INTEGER NOT NULL,
    end_us          INTEGER,
    start_frame     INTEGER,
    end_frame       INTEGER,
    text            TEXT NOT NULL DEFAULT '',
    geometry_json   TEXT,
    colour          TEXT,
    created_by      TEXT NOT NULL DEFAULT '',
    created_at      TEXT NOT NULL,
    modified_at     TEXT NOT NULL
);

CREATE INDEX idx_annotations_evidence_time ON annotations(evidence_id, start_us);
CREATE INDEX idx_annotations_case ON annotations(case_id);

-- Append-only audit trail. Deliberately free of foreign keys: identifiers are
-- denormalised so the record still reads correctly after a case or evidence row
-- is removed. The triggers below make UPDATE and DELETE fail at the database
-- level, not merely in the UI.
CREATE TABLE audit_events (
    id              TEXT PRIMARY KEY NOT NULL,
    sequence        INTEGER NOT NULL,
    occurred_at     TEXT NOT NULL,
    action          TEXT NOT NULL,
    actor           TEXT NOT NULL DEFAULT '',
    case_id         TEXT,
    case_number     TEXT,
    evidence_id     TEXT,
    evidence_number TEXT,
    description     TEXT NOT NULL DEFAULT '',
    details_json    TEXT NOT NULL DEFAULT '{}',
    outcome         TEXT NOT NULL DEFAULT 'success',
    app_version     TEXT NOT NULL DEFAULT '',
    host            TEXT NOT NULL DEFAULT ''
);

CREATE INDEX idx_audit_sequence ON audit_events(sequence);
CREATE INDEX idx_audit_case ON audit_events(case_id, sequence);
CREATE INDEX idx_audit_evidence ON audit_events(evidence_id, sequence);
CREATE INDEX idx_audit_action ON audit_events(action);

CREATE TRIGGER audit_events_block_update
BEFORE UPDATE ON audit_events
BEGIN
    SELECT RAISE(ABORT, 'audit_events is append-only: updates are not permitted');
END;

CREATE TRIGGER audit_events_block_delete
BEFORE DELETE ON audit_events
BEGIN
    SELECT RAISE(ABORT, 'audit_events is append-only: deletes are not permitted');
END;

CREATE TABLE application_settings (
    key         TEXT PRIMARY KEY NOT NULL,
    value       TEXT NOT NULL,
    value_type  TEXT NOT NULL DEFAULT 'string',
    updated_at  TEXT NOT NULL,
    updated_by  TEXT NOT NULL DEFAULT ''
);
