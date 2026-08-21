# Roadmap

Phase 0 is complete when a case can be created, evidence imported and hashed, played,
bookmarked, annotated, verified and audited — and all of it survives a restart. That is
what this repository contains. Everything below is future work, and none of it is
implemented.

## Deliberately not built in Phase 0

Facial recognition · identity matching · licence-plate recognition · person, object or
weapon detection · tracking · automatic incident reconstruction · natural-language
search · generative enhancement · gunshot identification · multi-camera tracking · 3D
reconstruction · city-camera networks · police database integration · cloud
infrastructure · live surveillance.

The UI lists these under **Modules**, disabled, so the direction is visible and nothing
pretends to work.

## The extension points that are already in place

| Later capability | What Phase 0 already provides |
|---|---|
| Any analysis model | `IAnalysisProvider` + `AnalysisProviderRegistry`; vendor-neutral request/result types |
| Detections, tracks, transcripts | `processing_operations` + `derived_assets` — no schema change needed |
| Detection and event overlays on the timeline | `TimelineWidget::setTracks` already draws point and range markers on arbitrary rows |
| Working copies and transcodes | `working/` directory and `DerivedAssetType::WorkingCopy` |
| GPU decode | Decoder is isolated behind `VideoDecoder`; the setting exists and is disabled |
| Multi-user deployments | `users` table, `UserRole`/`Permission` gate already enforced in services |
| Reports and exhibits | `reports/` and `exports/` directories; provenance chain answers what a report must cite |
| Multi-camera synchronisation | Positions are real presentation timestamps, per-stream detail is relational |

## Phase 1 — recommended starting point

**Audio.** It is the largest visible gap: the metadata is already extracted and shown,
the controls exist and are disabled, and the work is self-contained.

1. Decode audio alongside video in `PlaybackController` (a second decoder and a
   resampler; `libswresample` is already a dependency).
2. Render through `QAudioSink` (add `Qt6::Multimedia`), driven by the same presentation
   clock so audio and video stay locked.
3. Enable the volume and mute controls, and persist their state — the settings keys are
   already defined.
4. Add a waveform as a `TimelineTrack`, generated as a derived asset with provenance.

This exercises the playback engine, the timeline track architecture and the derived-asset
path in one piece of work, without touching the evidence core.

### Then, in rough order

- **Working copies** — a proportionate transcode for awkward codecs, recorded as a
  derived asset with the transcode parameters. Analysis then runs on a known format
  while the original stays untouched.
- **Export and reporting** — an exhibit bundle: selected frames and clips, their digests,
  their provenance chain, and the audit extract that supports them.
- **The first analysis provider** — pick one narrow, verifiable capability
  (transcription is a good first choice: the output is checkable by a human). Implement
  `IAnalysisProvider` out-of-process, persist findings as derived assets, surface them as
  a timeline track, and require analyst verification before anything appears in a report.
- **Local accounts** — password hashing and login on top of the existing `users` table
  and permission gate; the audit trail already records an actor and an account id.
- **Encrypted storage** — the managed layout and relative paths already make a
  per-case encrypted container feasible without touching the domain.

## Constraints later phases must keep

1. Original evidence is never modified. Ever.
2. Everything produced from evidence is a derived asset with its own digest and a
   complete provenance record.
3. Machine output is never self-certified; a human verifies before it becomes a finding.
4. The audit trail is append-only.
5. TRACE reports observations. It does not draw forensic conclusions.
