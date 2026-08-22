# Database

One SQLite database, `trace.db`, at the root of the data directory.

## Connection settings

Applied on every open (`core/database/database.cpp`):

| Pragma | Value | Why |
|---|---|---|
| `foreign_keys` | `ON` | Referential integrity is enforced, not assumed |
| `journal_mode` | `WAL` | Readers do not block the ingesting writer |
| `synchronous` | `FULL` | This file is the index of the evidence; a torn write loses provenance |
| `busy_timeout` | 5000 ms | Background workers and the UI share the connection |

The connection is serialised behind a recursive mutex. `Transaction` scopes nest via
`SAVEPOINT`, so repository-level transactions compose inside service-level ones.

## Tables

| Table | Holds | Cascade |
|---|---|---|
| `cases` | Investigations | — |
| `evidence` | Imported files | `case_id` → cases, ON DELETE CASCADE |
| `evidence_metadata` | 1:1 technical metadata + raw JSON | `evidence_id`, CASCADE |
| `media_streams` | One row per elementary stream | `evidence_id`, CASCADE |
| `processing_operations` | What was run over evidence | `case_id`, `evidence_id`, CASCADE |
| `derived_assets` | Outputs, with digests | `evidence_id` CASCADE, `operation_id` SET NULL |
| `bookmarks` | Marked moments | `evidence_id`, CASCADE |
| `annotations` | Timestamp and range notes | `evidence_id`, CASCADE |
| `analysis_runs` | One execution of an analysis over one item | `case_id`, `evidence_id`, CASCADE |
| `detections` | What a model reported, one row per object per analysed frame | `analysis_run_id`, `case_id`, `evidence_id`, CASCADE |
| `audit_events` | Append-only history | **no foreign keys — deliberately** |
| `application_settings` | Key/value settings | — |
| `users` | Local accounts (scaffolding) | — |
| `schema_migrations` | Applied versions and their checksums | — |

### Why `audit_events` has no foreign keys

The audit trail must outlive what it describes. Case and evidence identifiers are stored
as plain columns alongside denormalised `case_number` and `evidence_number`, so deleting
a case removes its content but leaves a readable history of what was done with it.

Two triggers make the table append-only at the database level:

```sql
CREATE TRIGGER audit_events_block_update BEFORE UPDATE ON audit_events
BEGIN SELECT RAISE(ABORT, 'audit_events is append-only: updates are not permitted'); END;

CREATE TRIGGER audit_events_block_delete BEFORE DELETE ON audit_events
BEGIN SELECT RAISE(ABORT, 'audit_events is append-only: deletes are not permitted'); END;
```

A defect in a future UI cannot rewrite history, and `AuditRepository` exposes no update
or delete to begin with. `sequence` is a monotonic counter so records keep their order
even when two share a millisecond.

### Why detections are relational rows

A detection could have been a JSON blob on the run. It is not, because an hour of
footage produces hundreds of thousands of them and every question an analyst asks is a
query: *by time* (what is on screen now), *by class or group* (show me the vehicles),
*by confidence* (hide anything under 60%), *by review state* (what have I not looked at).

Provenance is reached by joining the run rather than repeated on each row, so hundreds of
thousands of detections cannot disagree about which model produced them. See
[ANALYSIS_RUNS.md](ANALYSIS_RUNS.md).

Bounding boxes are stored **normalised to the source frame** (`bbox_x/y/w/h`, 0–1) with
pixel values alongside as a convenience. Normalised geometry stays correct when the item
is drawn at another size, which pixel coordinates do not.

`analysis_runs.sampling_interval_us` is a column rather than a field inside
`configuration_json` because the viewer needs it to decide which analysed frame belongs
to the playhead. Inferring that from the spacing of the detections would put boxes on the
wrong moment wherever a stretch of footage contained nothing.

## Indexes

Case listing and search (`status`, `modified_at`), evidence by case and by digest
(duplicate detection), bookmarks and annotations by `(evidence_id, position)` for
timeline rendering, derived assets by evidence and type, and audit by sequence, case,
evidence and action.

Detections carry six: `(evidence_id, timestamp_us)` for the overlay and the timeline,
`(analysis_run_id)`, `(case_id)`, `(evidence_id, class_group)`, `(evidence_id,
verification_state)` and `(evidence_id, confidence)` for the filters. Analysis runs carry
four: by case, by evidence (newest first), by status and by creation time.

## Migrations

`migrations/NNNN_description.sql`, applied in order and embedded into the binary at build
time by `scripts/embed_migrations.cmake` — a deployed build never needs loose SQL files.

| Version | File | Contents |
|---|---|---|
| 1 | `0001_initial_schema.sql` | Cases, evidence, metadata, streams, provenance, bookmarks, annotations, audit, settings |
| 2 | `0002_security_foundation.sql` | `users`, and `audit_events.actor_user_id` |

Each applied migration records its SHA-256. On every startup those digests are compared
with the migrations compiled into the running binary:

- **Digest mismatch** → the database and the code no longer agree about the schema.
  TRACE refuses to continue rather than operating on a schema it cannot describe.
- **Unknown newer version** → the database was created by a newer build; TRACE says so
  instead of downgrading it.

### Adding a migration

1. Add `migrations/0003_<description>.sql` — forward-only; never edit an applied file.
2. Update the model struct, the repository column list and its reader.
3. Extend `tests/unit/database_test.cpp` and the relevant repository test.
4. Rebuild: the embedding step picks the file up automatically.

## Conventions

- Timestamps: ISO-8601 UTC with milliseconds (`2026-08-21T04:51:41.157Z`), so text
  ordering is chronological ordering.
- Media positions: integer microseconds.
- Unknown values: `NULL`. Never `0`, never `''`.
- JSON columns (`raw_metadata_json`, `details_json`, `parameters_json`,
  `library_versions`, `geometry_json`) are for semi-structured payloads only; anything
  queried or joined gets a real column.
