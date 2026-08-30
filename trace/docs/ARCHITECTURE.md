# TRACE architecture

This document is for the next engineer (human or otherwise) to pick up TRACE without
reverse-engineering Phase 0. It describes the module boundaries, the rules those
boundaries encode, and where new work belongs.

## Layering

```
              ┌───────────────────────┐
              │   apps/desktop        │  entry point, command line, theme
              └───────────┬───────────┘
                          │
              ┌───────────▼───────────┐
              │         ui/           │  Qt widgets only
              │  ApplicationContext   │  the single seam between UI and domain
              └───┬───────┬───────┬───────┬───┘
                  │       │       │       │
      ┌───────────▼──┐ ┌──▼────┐ ┌▼─────┐ ┌▼───────────┐
      │    core/     │ │media/ │ │ ai/  │ │  analysis/ │
      │ no Qt        │ │FFmpeg │ │models│ │  pipeline  │
      │ no FFmpeg    │ │no Qt  │ │no Qt │ │ no Qt/SQL  │
      └──────────────┘ └───────┘ └──────┘ └────────────┘
             ▲             │        │            │
             │             │        │            │
             └─────────────┴────────┴────────────┘
              media, ai and analysis all depend on core;
              analysis additionally depends on media and ai
```

Rules the build enforces:

- **`core` knows nothing about Qt or FFmpeg.** It is the evidence domain: models,
  repositories, services, storage, hashing, audit. It links only SQLite and the standard
  library, so it can be tested headlessly and reused by a future CLI, service or agent.
- **`media` knows nothing about Qt.** It hands decoded frames and metadata back through
  plain structs and callbacks.
- **`ui` contains no SQL, no hashing and no decoding.** Widgets call services through
  `ApplicationContext`. If a widget needs to compute something about evidence, that
  computation belongs in a service.
- **`ai` names no vendor above its `providers/` directory.** `IDetectionProvider`, the
  registry, the model catalogue and the pre/post-processing maths are runtime-neutral;
  the only file that includes an ONNX Runtime header is
  `ai/detection/providers/onnx_detection_provider.cpp`.
- **`analysis` contains no Qt and no SQL.** It joins the three layers below it — media
  decodes, ai detects, core persists and audits — so the whole detection path is
  testable headlessly.

The detection seam is `IDetectionProvider` (`ai/detection/detection_provider.h`): the
pipeline declares what it needs — one decoded frame in, observations in source-frame
normalised coordinates out — and a provider supplies it. Adding a runtime is adding a
class; see [AI_ARCHITECTURE.md](AI_ARCHITECTURE.md).

The seam between the media layer and the domain is `IMetadataExtractor`
(`core/services/metadata_extractor.h`): the domain declares what it needs, and
`media/ffmpeg/media_probe.cpp` provides it. Tests substitute their own.

## Error handling

Domain code does not throw across module boundaries. Every operation that can fail
returns `Result<T>` or `Status` (`core/common/result.h`), each carrying an `ErrorCode`,
a message written for an operator, and optional technical detail for the log.

Three rules follow from evidence work:

1. **No silent failure.** A failed import, hash or verification is returned, logged and
   audited.
2. **Cancellation is a failure, not a success.** It surfaces as `ErrorCode::Cancelled`.
3. **Integrity and decodability are different.** A file whose container cannot be parsed
   is `MediaStatus::MetadataFailed` with `IntegrityStatus::Verified` — intact bytes that
   TRACE cannot decode. Never conflate them.

## Threading

The UI thread never blocks on evidence work.

| Work | Where it runs | How results return |
|---|---|---|
| Import (copy, hash, verify, probe, insert) | `BackgroundTask` worker thread | Queued Qt signals to `IngestDialog` |
| Integrity verification | `BackgroundTask` worker thread | Queued signals to `InspectorPanel` |
| Frame export, preview generation | `BackgroundTask` worker thread | Queued signals to `MainWindow` |
| Decoding, seeking, playback pacing | `PlaybackController`'s own thread | Callbacks, marshalled by `PlaybackBridge` |

`PlaybackController` (`media/playback`) is deliberately Qt-free: it owns a command queue,
a decoder and a private thread, and calls back from that thread. `ui/viewer/PlaybackBridge`
is the only place those callbacks become Qt signals, via `QMetaObject::invokeMethod` with
`Qt::QueuedConnection`.

Audio adds a third thread and inverts which clock is authoritative. `ui/audio/AudioOutput`
runs a `QAudioSink` on its own thread, and `PlaybackController::setClockSource()` lets the
video pacer follow the device's position rather than the steady clock while sound is
playing. The arithmetic that turns a device's count into a media position lives in
`media/audio/AudioClock` — no Qt, so it is testable on a machine with no sound card. See
`docs/AUDIO.md`.

Encryption sits underneath all of it rather than beside it. `core/security/crypto`
provides the container format and the key hierarchy; `core/security/WorkspaceKeys` holds
the master key for an open session and derives a key per case; `media/ffmpeg/EncryptedMediaIo`
lets FFmpeg read a container through a custom `AVIOContext` so no plaintext copy is ever
written to disk. Every path that decodes, hashes or exports evidence takes a
`const crypto::SecretKey*`, where null means "this file is not encrypted" — a workspace can
hold both forms at once, and readers decide per file by looking at it. See `docs/ENCRYPTION.md`.

Two hazards this design has already hit, both now covered by fixes and tests:

- A background task that finishes *after* the context has shut down must not call into
  released services. `MainWindow::closeEvent` cancels, joins **and destroys** its tasks
  before shutdown, `ApplicationContext` drops notifications once shut down, and panels
  check `isInitialised()`.
- Refreshing a list re-selects the current row. That must not be treated as the operator
  opening an item, or playback restarts underneath them.

## Database access

One `Database` object wraps one SQLite connection, serialised behind a recursive mutex so
worker threads and the UI can share it. `Transaction` scopes nest: an inner scope becomes
a `SAVEPOINT`, so a repository that wraps its own writes can be called from inside a
service transaction. Every value reaches SQLite through a bind call; TRACE never
concatenates user text into SQL.

## Adding to TRACE

| To add… | Do this |
|---|---|
| A field on an existing entity | New migration in `migrations/`, extend the model struct, the repository column list and reader, then the inspector |
| A new entity | Migration, model, repository, then a service that owns its rules and writes audit records |
| A new evidence operation (clip, transcode, enhancement) | Write into the case's managed storage, then register through `DerivedAssetService::registerAsset` so it inherits hashing, provenance and audit |
| A new analysis capability | Implement `IAnalysisProvider`, register it; persist findings as derived assets/operations. Do not add vendor code to `core` |
| A new timeline row (detections, events) | Push another `TimelineTrack` — `TimelineWidget` draws point markers, range markers and amplitude envelopes, and knows nothing about bookmarks |
| A new UI panel | Take `ApplicationContext*`; call services; keep queries out of paint and event handlers |

## Conventions

- Media positions are **signed microseconds** (`trace::Microseconds`), never frame
  indices. Frame numbers are display-only and are omitted when the stream's timing is
  variable.
- Wall-clock timestamps are ISO-8601 UTC with milliseconds, so they sort
  lexicographically in SQLite.
- Absent information is `std::nullopt` and displays as "Not available" — never `0`,
  never an empty string standing in for a real value.
- Identity is a UUID. Display numbers (`CASE-0001`, `EVD-000001`) are labels.
- Log paths, sizes, identifiers and digests; never evidence content.
