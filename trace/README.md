# TRACE — Phase 1

**TRACE** is a video evidence intelligence platform for investigators, analysts and
authorised agencies. This repository contains **Phase 0** — the evidence foundation — and
**Phase 1**, its first genuine computer-vision capability: object detection.

It is a working desktop application, not a prototype. It ingests evidence into managed
storage, hashes it, extracts real technical metadata, plays it back with frame-accurate
control, records bookmarks and notes, exports frames as derived assets with full
provenance, runs a real detection model over a recording and lets an analyst review
every result — and keeps an append-only audit trail of all of it.

**TRACE reports observations and makes no forensic determinations.** Object detection
states a visual class and the model's confidence in it. It does not identify individuals,
read number plates, track anyone, or say what was happening. Conclusions remain the
analyst's.

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
| **Local accounts and sign-in** — PBKDF2-HMAC-SHA256, lockout, audited | Working |
| Role gate with teeth: managing accounts requires Administrator | Working |
| **Object detection** over a recording, with a real model | Working |
| Provider and model selection; model SHA-256 verified before the run | Working |
| Detection overlay on the video, with label/confidence switches | Working |
| Timeline lanes for People, Vehicles and Objects | Working |
| Detections table with class, group, confidence and review filters; click to jump | Working |
| Detection inspector: class, confidence, timestamp, box, evidence, model, digest, device, run | Working |
| Human verification — confirmed / rejected / uncertain, audited, never destructive | Working |
| Keyboard-driven review with auto-advance, and review progress per run | Working |
| Bulk confirm/reject across a filter — recorded as a bulk decision, not as N reviews | Working |
| Cancel a running analysis; partial results kept and the run recorded as cancelled | Working |
| **Audio playback** | **Written, unproven** — implemented through `QAudioSink`; no machine with an audio device has run it. See `docs/AUDIO.md` §5 |
| Audio decoding and resampling (libswresample) | Working |
| Waveform envelope on the timeline, generated as a derived asset with provenance | Working |
| Volume and mute, persisted; neither alters evidence | Working |
| **Hardware-accelerated decode** | **Not implemented** — setting exists and is disabled; TRACE decodes in software |
| **GPU inference (CUDA)** | **Written and guarded, never executed here** — this environment has no GPU; see `docs/PHASE1_TESTING.md` |
| **Face recognition, identity, plates, tracking, re-identification** | **Not implemented, by design** — see `docs/ROADMAP.md` |

| **Encryption at rest** — case database and evidence | Working | SQLCipher for the database, AES-256-GCM containers for the recordings. Derived assets are still in the clear; see `docs/ENCRYPTION.md` §6 |
| Encrypting an existing workspace | Working | `--encrypt-workspace`; resumable, and verified against each item's ingestion digest before its original is replaced |
| More than one operator per encrypted workspace | Working | Each holds their own wrapped copy of the master key; a password change re-wraps 32 bytes, not the case load |

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
  ai/                  Model-facing code, no Qt
    interfaces/        IAnalysisProvider, request/result types, provider registry
    detection/         IDetectionProvider, model catalogue, letterbox, NMS, providers
  analysis/            Detection pipeline: sampling, threading, batching. No Qt, no SQL
  ui/                  Qt widgets only; all domain work goes through ApplicationContext
    analysis/          Analysis panel, detections table, detection inspector
  migrations/          Versioned schema (*.sql), embedded into the binary
  models/              Detection artefacts (git-ignored, fetched by script)
  third_party/         ONNX Runtime (git-ignored, fetched by script)
  tests/               unit/, integration/, fixtures/, support/
  docs/                Architecture, evidence model, provenance, database, AI, roadmap
  scripts/             Migration embedding, sample media, runtime/model/media fetchers
```

The dependency direction is strict and enforced by the build:

```
apps/desktop → ui → { core, media, ai, analysis }
                       analysis → { core, media, ai }
                       media    → core
                       ai       → core
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
| ONNX Runtime | 1.17.3 (CPU package) | Optional but needed for real detection; fetched by `scripts/fetch_onnxruntime.sh`, not vendored |
| OpenCV | optional, off by default | `-DTRACE_WITH_OPENCV=ON` reserved for later image work |

### Linux / macOS

```bash
# Debian/Ubuntu dependencies
sudo apt-get install -y build-essential cmake ninja-build \
    qt6-base-dev qt6-base-dev-tools libgl1-mesa-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
    libsqlite3-dev libgtest-dev libgmock-dev

# Optional, and needed for real detection: the runtime and a model.
./trace/scripts/fetch_onnxruntime.sh          # add --gpu for the CUDA package
./trace/scripts/fetch_models.sh               # YOLOX-Tiny, SHA-256 verified

cmake -S trace -B trace/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DTRACE_WITH_ONNXRUNTIME=ON \
      -DTRACE_ONNXRUNTIME_ROOT=trace/third_party/onnxruntime
cmake --build trace/build -j"$(nproc)"
ctest --test-dir trace/build --output-on-failure
./trace/build/bin/trace
```

Without ONNX Runtime the application still builds and runs: only the deterministic mock
provider is offered, and the analysis panel says so.

### Windows 11

See **[docs/BUILD_WINDOWS.md](docs/BUILD_WINDOWS.md)** for the vcpkg-based instructions,
including the exact triplet and the DLL deployment step.

### Build options

| Option | Default | Effect |
|---|---|---|
| `TRACE_BUILD_DESKTOP` | `ON` | Build the Qt application; `OFF` builds only the headless libraries |
| `TRACE_BUILD_TESTS` | `ON` | Build the three test suites |
| `TRACE_WITH_ONNXRUNTIME` | `OFF` | Build the ONNX Runtime detection provider |
| `TRACE_ONNXRUNTIME_ROOT` | — | Where the runtime package was unpacked |
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

Detection models are looked for in `<data root>/models`, overridable with
`TRACE_MODEL_DIR`. **TRACE never downloads a model.** A model that is not installed is
reported as missing, with the path it was expected at and the script that fetches it.

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

Analysis does not change this. A detection run **reads** the managed original and writes
rows to `analysis_runs` and `detections`; it never re-encodes the video, never burns boxes
into it, and never touches the evidence's recorded SHA-256. The overlay is drawn onto the
widget, not into the frame — there is a unit test that compares the decoded frame's bytes
before and after the boxes are drawn. The acceptance tests hash the managed original after
every run and require it to be identical.

---

## Tests

```bash
ctest --test-dir trace/build --output-on-failure
```

124 test cases in five suites:

- **`trace_unit_tests`** (81) — SHA-256 against published NIST vectors, identifier and
  timecode handling, JSON escaping, managed storage paths, migrations and drift
  detection, the append-only audit guarantee, repository round-trips, service
  validation, and the Phase 1 detection maths: the letterbox transform and its inverse,
  class-aware NMS, the YOLOX grid decode against a hand-computed tensor, the model
  catalogue, timestamp-driven sampling and the run-status rules.
- **`trace_ui_unit_tests`** (6) — overlay geometry under letterboxing, hit-testing,
  colour semantics, and a byte-level check that drawing boxes leaves the decoded frame
  untouched.
- **`trace_integration_tests`** (35) — ingestion against the real sample file,
  persistence across a restart, integrity failure detection, decoding, accurate seeking,
  frame stepping, playback, frame export, and the whole detection pipeline including
  real inference against real footage.
- **`trace_acceptance_test`** (1) — the Phase 0 §37 workflow driven through the real
  application. Set `TRACE_SCREENSHOT_DIR` to capture screenshots of each stage.
- **`trace_acceptance_phase1_test`** (1) — the Phase 1 workflows through the real window:
  a successful analysis with review and restart, a failed analysis, and a cancelled one.

Both GUI suites run headless (`QT_QPA_PLATFORM=offscreen`).

The sample clip in `tests/fixtures/sample.mp4` is generated from FFmpeg's synthetic
sources by `scripts/make_sample_video.sh` — no third-party footage is bundled. Tests that
need a real model or real street footage fetch them by script and **skip themselves,
saying why, when they are absent**.

See **[docs/PHASE1_TESTING.md](docs/PHASE1_TESTING.md)** for what was measured and, just
as importantly, what was not.

---

## Documentation

| Document | Contents |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Module boundaries, threading, error handling, where to add code |
| [docs/EVIDENCE_MODEL.md](docs/EVIDENCE_MODEL.md) | Case and evidence records, storage layout, integrity states |
| [docs/PROVENANCE.md](docs/PROVENANCE.md) | The chain every derived result must satisfy |
| [docs/DATABASE.md](docs/DATABASE.md) | Schema, keys, cascade policy, migrations |
| [docs/BUILD_WINDOWS.md](docs/BUILD_WINDOWS.md) | Windows 11 build with vcpkg |
| [docs/AI_ARCHITECTURE.md](docs/AI_ARCHITECTURE.md) | How analysis plugs in, the provider interface, threading, what analysis may touch |
| [docs/DETECTION_MODEL.md](docs/DETECTION_MODEL.md) | The model, preprocessing, output decode, class grouping, measured performance |
| [docs/ANALYSIS_RUNS.md](docs/ANALYSIS_RUNS.md) | The run record, its statuses, and the rules that keep it honest |
| [docs/MODEL_PROVENANCE.md](docs/MODEL_PROVENANCE.md) | The chain from a box on screen to the bytes of the model that drew it |
| [docs/PHASE1_TESTING.md](docs/PHASE1_TESTING.md) | Phase 1 coverage, the defects it caught, and what was not tested |
| [docs/REPORTING.md](docs/REPORTING.md) | Exhibit bundles, what a report may and may not say, clips |
| [docs/EXHIBIT_VERIFICATION.md](docs/EXHIBIT_VERIFICATION.md) | How anyone verifies a bundle without TRACE |
| [docs/PHASE2_TESTING.md](docs/PHASE2_TESTING.md) | Phase 2 coverage and what was not tested |
| [docs/AUDIO.md](docs/AUDIO.md) | Audio decoding, the waveform, which clock playback follows, and what is unproven |
| [docs/PHASE3_TESTING.md](docs/PHASE3_TESTING.md) | Phase 3 coverage and the gap where a sound card would be |
| [docs/AUTHENTICATION.md](docs/AUTHENTICATION.md) | How passwords are stored, why not SHA-256, and what local accounts do *not* protect |
| [docs/ENCRYPTION.md](docs/ENCRYPTION.md) | What is encrypted, the key hierarchy, and §6: what encryption at rest does *not* protect |
| [docs/ROADMAP.md](docs/ROADMAP.md) | What Phase 2+ adds and the extension points waiting for it |

---

## Licence and scope

TRACE is intended for lawful, authorised evidence analysis. It reports what files
contain and what was done to them; it does not draw conclusions, and no output of this
software should be presented as a forensic determination on its own.
