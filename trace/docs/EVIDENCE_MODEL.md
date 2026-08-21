# The TRACE evidence model

## The rule

> **TRACE never modifies original evidence.**

Everything below exists to make that rule true in practice and demonstrable afterwards.

## Case

A case is the container for an investigation. Every other record carries its identifier.

| Field | Notes |
|---|---|
| `id` | UUID — identity |
| `case_number` | Human-readable label, unique (`CASE-0001` by default, editable) |
| `title`, `description`, `notes` | Free text |
| `status` | open, active, on_hold, closed, archived |
| `investigator`, `agency`, `location` | Optional |
| `created_at`, `modified_at`, `created_by` | ISO-8601 UTC |
| `storage_relpath` | `cases/<uuid>`, relative to the data directory |

Creating a case makes its storage directories **before** the row is written: a case that
points at directories which do not exist is worse than no case at all.

## Evidence

| Field | Notes |
|---|---|
| `id` | UUID — identity |
| `evidence_number` | `EVD-000001`, unique within the case |
| `original_filename` | As supplied by the operator |
| `stored_filename` | `<compact-uuid>_<sanitised-name>.<ext>` — collision-safe |
| `source_path` | Where it came from, for the chain of custody. Never written to |
| `storage_relpath` | Managed original, relative to the data directory |
| `media_type` | From the container's actual streams, extension only as a fallback |
| `mime_type`, `file_size`, `sha256` | `sha256` is the digest of the managed original |
| `source_modified_at` | The source file's mtime at ingestion, when the OS reported one |
| `ingested_at`, `modified_at`, `ingested_by` | |
| `integrity_status`, `last_verified_at` | See below |
| `media_status` | Decodable, metadata unreadable, codec unsupported, not media |

### Ingestion, step by step

1. **Validate** — case exists, source exists and is readable.
2. **Prepare storage** — create the case directory set if needed.
3. **Copy and hash** — stream the source into managed storage in 1 MiB chunks, hashing
   as it is written. The source is opened read-only.
4. **Verify the copy** — re-read what landed on disk and hash it independently. Size and
   digest must both match, or the copy is deleted and the import fails.
5. **Extract metadata** — best effort. Failure here sets `media_status` and never fails
   the import or affects `integrity_status`.
6. **Create records** — evidence, metadata and stream rows in one transaction.
7. **Audit** — import started, hashed, imported, metadata extracted or failed.

Any fatal failure after step 3 removes the managed copy and leaves no evidence row. The
operator's file is untouched in every path, including cancellation.

### Storage layout

```
TRACE_DATA/
  trace.db
  logs/
  cases/<case-uuid>/
    originals/     immutable ingested files, marked read-only
    working/       analysis-safe derivations
    thumbnails/    generated previews
    exports/       frame exports and other operator output
    reports/
    logs/
```

Paths are stored **relative** to the data directory, so a case can be moved to another
drive or workstation without invalidating every row.

### Integrity states

| State | Meaning |
|---|---|
| `NOT VERIFIED` | Nothing has been re-checked since ingestion |
| `VERIFIED` | The managed original matched its recorded digest at `last_verified_at` |
| `INTEGRITY FAILURE` | It did not match. The recorded digest is unchanged |
| `VERIFYING` | A check is running |
| `ERROR` | The check could not run — for example the managed file is missing |

A verification never rewrites `sha256`. A missing file is an `ERROR`, not corruption:
TRACE reports what it observed and leaves interpretation to the analyst.

## Metadata

`evidence_metadata` holds the fields TRACE displays and queries; `media_streams` holds
one row per elementary stream. `raw_metadata_json` preserves everything the container
reported, verbatim, pretty-printed.

Every field is optional. A container that carries no GPS produces no GPS row, and the
inspector prints "Not available". Frame rate is reported as **average** and **nominal**
separately, with a `frame_rate_mode` of `constant_suspected` or `variable` derived from
the difference between them — TRACE does not assume constant frame rate, because
timestamps computed from that assumption are wrong for most body-worn and phone footage.

## Bookmarks and annotations

Both are anchored to `timestamp_us` / `start_us`, the real presentation timestamp of the
frame the analyst was looking at. `frame_number` is recorded only when the stream's
timing is regular enough for an index to mean anything.

Annotations carry a type (`timestamp_note`, `range_note` in Phase 0; `point`, `region`,
`arrow`, `measurement` reserved) and an optional `geometry_json`, so drawing tools land
as new geometry on the existing record rather than a new table.
