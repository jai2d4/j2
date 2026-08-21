# TRACE — Phase 0

**TRACE** is a video evidence intelligence platform for investigators, analysts and
authorised agencies. This repository contains **Phase 0**: the evidence foundation that
every later analysis module plugs into.

Phase 0 is a working desktop application, not a prototype. It ingests evidence into
managed storage, hashes it, extracts real technical metadata, plays it back with
frame-accurate control, records bookmarks and notes, exports frames as derived assets
with full provenance, and keeps an append-only audit trail of everything that happened.

**Phase 0 performs no AI analysis and makes no forensic determinations.** It ships the
provider interface for later phases and nothing else. Conclusions remain the analyst's.

---

## What actually works

| Capability | State |
|---|---|
| Create, edit, search and reopen cases | Working |
| Import evidence into managed per-case storage | Working |
| SHA-256 of the managed original, computed at ingestion and re-verified from disk | Working |
| Copy verified byte-for-byte against the source before the record is created | Working |
| Technical metadata via libavformat (container, streams, codecs, timing, GPS when present) | Working |
| Raw container metadata preserved as JSON | Working |
| Video playback: play, pause, stop, seek, jump, frame step, 0.25×–4× speed | Working |
| Frame-accurate seeking and frame stepping on variable-frame-rate media | Working |
| Timeline with playhead, zoom, bookmark and note markers | Working |
| Bookmarks and timestamp/range notes, persisted per evidence item | Working |
| Save current frame as a PNG derived asset with hash and provenance | Working |
| Preview image generation (registered as a derived asset) | Working |
| Verify integrity, with VERIFIED / INTEGRITY FAILURE reported and audited | Working |
| Append-only audit trail, enforced by database triggers | Working |
| Versioned schema migrations with drift detection | Working |
| Settings, with capability detection | Working |
| **Audio playback** | **Not implemented** — controls are visibly disabled; audio stream metadata *is* extracted and shown |
| **Hardware-accelerated decode** | **Not implemented** — setting exists and is disabled; Phase 0 decodes in software |
| **Any AI/analysis module** | **Not implemented by design** — see `ai/interfaces` and `docs/ROADMAP.md` |

Anything not listed as working is either absent or shown disabled with the reason.
There are no placeholder screens and no simulated results.

---

## Repository layout

```
trace/
  apps/desktop/        Application entry point
  core/                Domain: no Qt, no FFmpeg
    common/            Result/Error, UUID, time, JSON, logging, strings
    security/          SHA-256, streaming file hashing, roles and permissions
    database/          SQLite wrapper, migration runner (SQL embedded at build time)
    models/            Case, Evidence, Metadata, Bookmark, Annotation, DerivedAsset, Audit
    repositories/      One repository per entity; SQL lives here and nowhere else
    services/          Case, ingestion, integrity, annotation and provenance services
    storage/           Managed storage layout, verified copy
    settings/          Typed settings, workstation capability detection
  media/               FFmpeg-backed, no Qt
    ffmpeg/            Metadata probe, decoder (accurate seek, frame stepping)
    playback/          Threaded playback engine driven by presentation timestamps
    thumbnails/        PNG writer, frame export and preview services
  ai/interfaces/       IAnalysisProvider, request/result types, provider registry
  ui/                  Qt widgets only; all domain work goes through ApplicationContext
  migrations/          Versioned schema (*.sql), embedded into the binary
  tests/               unit/, integration/, fixtures/, support/
  docs/                Architecture, evidence model, provenance, database, roadmap
  scripts/             Migration embedding, sample media generation
```

The dependency direction is strict and enforced by the build:

```
apps/desktop → ui → { core, media, ai }
                       media → core
                       ai    → core
```

`core` and `media` contain no Qt and are exercised headlessly by the test suites.

---

## Building

### Dependencies

| Component | Version used in development | Notes |
|---|---|---|
| CMake | 3.28.3 | 3.21 is the declared minimum |
| C++ compiler | GCC 13.3 (C++20) | MSVC 19.3x and Clang 16+ are supported by the code |
| Qt | 6.4.2 (Core, Gui, Widgets, Concurrent) | Widgets only; no QML, no Qt Multimedia |
| FFmpeg | 6.1.1 — libavformat 60.16.100, libavcodec 60.31.102, libavutil 58.29.100, libswscale 7.5.100, libswresample 4.12.100 | Decoding, probing and PNG encoding |
| SQLite | 3.45.1 | System library |
| GoogleTest | system package | Tests only |
| OpenCV | optional, off by default | `-DTRACE_WITH_OPENCV=ON` reserved for later image work |

### Linux / macOS

```bash
# Debian/Ubuntu dependencies
sudo apt-get install -y build-essential cmake ninja-build \
    qt6-base-dev qt6-base-dev-tools libgl1-mesa-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
    libsqlite3-dev libgtest-dev libgmock-dev

cmake -S trace -B trace/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build trace/build -j"$(nproc)"
ctest --test-dir trace/build --output-on-failure
./trace/build/bin/trace
```

### Windows 11

See **[docs/BUILD_WINDOWS.md](docs/BUILD_WINDOWS.md)** for the vcpkg-based instructions,
including the exact triplet and the DLL deployment step.

### Build options

| Option | Default | Effect |
|---|---|---|
| `TRACE_BUILD_DESKTOP` | `ON` | Build the Qt application; `OFF` builds only the headless libraries |
| `TRACE_BUILD_TESTS` | `ON` | Build the three test suites |
| `TRACE_WITH_OPENCV` | `OFF` | Link optional OpenCV helpers |

---

## Running

```bash
./trace/build/bin/trace                      # uses the platform default data directory
./trace/build/bin/trace -d /path/to/TRACE_DATA
TRACE_DATA_DIR=/path/to/TRACE_DATA ./trace/build/bin/trace
```

Default data directories:

| Platform | Location |
|---|---|
| Windows | `%LOCALAPPDATA%\TRACE\TRACE_DATA` |
| macOS | `~/Library/Application Support/TRACE/TRACE_DATA` |
| Linux | `${XDG_DATA_HOME:-~/.local/share}/TRACE/TRACE_DATA` |

The data directory holds `trace.db`, the application log and one folder per case
(`originals/`, `working/`, `thumbnails/`, `exports/`, `reports/`, `logs/`).

---

## The rule that governs everything

> **TRACE never modifies original evidence.**

At ingestion the source file is opened read-only and copied into managed storage. The
copy is hashed while it is written and then re-read and hashed again to prove it matches
the source; only then is the evidence record created. The managed original is marked
read-only and is never written to again. Transcodes, frame exports, previews, and every
future analysis output are **derived assets** that carry their own digest and a
provenance record naming the operation, parameters, software version, source timestamp
and operator.

A failed integrity check never overwrites the recorded digest. It is reported, persisted
as a failed status, and written to the audit trail for a human to interpret.

---

## Tests

```bash
ctest --test-dir trace/build --output-on-failure
```

80 test cases in three suites:

- **`trace_unit_tests`** (55) — SHA-256 against published NIST vectors, identifier and
  timecode handling, JSON escaping, managed storage paths, migrations and drift
  detection, the append-only audit guarantee, repository round-trips, service
  validation and the analysis registry.
- **`trace_integration_tests`** (24) — ingestion against the real sample file,
  persistence across a restart, integrity failure detection, decoding, accurate seeking,
  frame stepping, playback control, frame export and preview generation.
- **`trace_acceptance_test`** (1) — the §37 workflow driven through the real
  application: create a case, import, play, seek, step, bookmark, close, reopen, verify
  integrity, read the audit trail. Runs headless (`QT_QPA_PLATFORM=offscreen`); set
  `TRACE_SCREENSHOT_DIR` to capture screenshots of each stage.

The sample clip in `tests/fixtures/sample.mp4` is generated from FFmpeg's synthetic
sources by `scripts/make_sample_video.sh` — no third-party footage is bundled.

---

## Documentation

| Document | Contents |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Module boundaries, threading, error handling, where to add code |
| [docs/EVIDENCE_MODEL.md](docs/EVIDENCE_MODEL.md) | Case and evidence records, storage layout, integrity states |
| [docs/PROVENANCE.md](docs/PROVENANCE.md) | The chain every derived result must satisfy |
| [docs/DATABASE.md](docs/DATABASE.md) | Schema, keys, cascade policy, migrations |
| [docs/BUILD_WINDOWS.md](docs/BUILD_WINDOWS.md) | Windows 11 build with vcpkg |
| [docs/ROADMAP.md](docs/ROADMAP.md) | What Phase 1+ adds and the extension points waiting for it |

---

## Licence and scope

TRACE is intended for lawful, authorised evidence analysis. It reports what files
contain and what was done to them; it does not draw conclusions, and no output of this
software should be presented as a forensic determination on its own.
