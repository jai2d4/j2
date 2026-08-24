# Roadmap

**Phase 0** is the evidence foundation: a case can be created, evidence imported and
hashed, played, bookmarked, annotated, verified and audited, and all of it survives a
restart.

**Phase 1** is object detection: an operator can analyse a video with a real model,
review what it found frame by frame, and answer where every box came from.

**Phase 2** is reports and exhibit export: a case and its confirmed findings become a
self-contained bundle that a third party can verify without TRACE, using nothing but the
SHA-256 tool their operating system already ships.

All three are complete and in this repository. Everything below Phase 2 is future work,
and none of it is implemented.

---

## What Phase 1 added

| Capability | Where |
|---|---|
| `IDetectionProvider` and a factory registry — the vendor-neutral extension point | `ai/detection/` |
| ONNX Runtime provider, CPU with a guarded CUDA path | `ai/detection/providers/onnx_detection_provider.cpp` |
| Deterministic mock provider so UI and database tests need no model or GPU | `ai/detection/providers/mock_detection_provider.cpp` |
| Model catalogue, digest verification, "never downloads" policy | `ai/detection/models/model_manager.cpp` |
| Letterbox transform and its inverse; grid decode; class-aware NMS | `ai/detection/preprocessing/`, `postprocessing/` |
| Timestamp-driven frame sampling (never `frame ÷ fps`) | `analysis/frame_sampler.cpp` |
| Threaded pipeline: bounded queue, batched writes, per-frame cancellation | `analysis/analysis_pipeline.cpp` |
| `analysis_runs` and `detections` tables | `migrations/0003_analysis_and_detections.sql` |
| Analysis panel, detections table, detection inspector, video overlay, timeline lanes | `ui/analysis/`, `ui/viewer/`, `ui/timeline/` |
| Human verification: confirmed / rejected / uncertain, audited, never destructive | `core/services/analysis_service.cpp` |

Detection docs: [AI_ARCHITECTURE](AI_ARCHITECTURE.md) ·
[DETECTION_MODEL](DETECTION_MODEL.md) · [ANALYSIS_RUNS](ANALYSIS_RUNS.md) ·
[MODEL_PROVENANCE](MODEL_PROVENANCE.md) · [PHASE1_TESTING](PHASE1_TESTING.md).

---

## Deliberately not built

Facial recognition · biometric templates · identity matching or naming a person ·
licence-plate recognition · person re-identification · object tracking across frames ·
cross-camera identity · suspect designation · weapon classification as a conclusion ·
automatic incident reconstruction · natural-language search over footage · generative
enhancement · gunshot identification · 3D reconstruction · city-camera networks · police
database integration · live surveillance.

Some of these are later phases. Several of them are permanently out of scope for this
software: TRACE says *"person detected"*, and it will not be extended to say
*"this is John Smith"*.

The UI lists what is genuinely planned-but-absent under **Modules**, disabled, so the
direction is visible and nothing pretends to work. Object detection left that list in
Phase 1 — it is a real menu action now.

---

## The extension points already in place

| Later capability | What already exists |
|---|---|
| Another detection runtime (TensorRT, OpenVINO, a remote service) | `IDetectionProvider` + registry; no core, UI, storage or schema change needed |
| Another analysis *type* (audio events, scene classification) | `analysis_runs.analysis_type`, its own results table keyed to the run, a new pipeline in `analysis/` |
| More timeline lanes | `TimelineWidget::setTracks` already draws point and range markers on arbitrary rows; it knows nothing about detections |
| Working copies and transcodes | `working/` directory and `DerivedAssetType::WorkingCopy` |
| GPU decode | Decoder is isolated behind `VideoDecoder`; the setting exists and is disabled |
| Multi-user deployments | `users` table, `UserRole`/`Permission` gate already enforced in services, including detection review |
| Reports and exhibits | `reports/` and `exports/` directories; the run record answers what a report must cite |
| Multi-camera synchronisation | Positions are real presentation timestamps throughout |

---

## Phase 3 — recommended next

**Audio.** It is now the largest visible gap: the metadata is already extracted and
shown, the controls exist and are disabled, and the work is self-contained.

1. Decode audio alongside video in `PlaybackController` (a second decoder and a
   resampler; `libswresample` is already a dependency).
2. Render through `QAudioSink` (add `Qt6::Multimedia`), driven by the same presentation
   clock so audio and video stay locked.
3. Enable the volume and mute controls and persist their state — the settings keys exist.
4. Add a waveform as a `TimelineTrack`, generated as a derived asset with provenance.

### Then, in rough order

- **Detection review at scale** — keyboard-driven review, bulk confirm/reject within a
  time range and class, and a review-progress indicator per run.
- **GPU verification** — the CUDA path is written and guarded but has never executed on a
  GPU here. It needs a machine with an NVIDIA device, a GPU package of ONNX Runtime and a
  measured comparison before any GPU performance claim is made.
- **Working copies** — a proportionate transcode for awkward codecs, recorded as a
  derived asset with its parameters, so analysis runs on a known format while the
  original stays untouched.
- **Local accounts** — password hashing and login on top of the existing `users` table
  and permission gate.
- **Encrypted storage** — the managed layout and relative paths already make a per-case
  encrypted container feasible without touching the domain.

---

## Constraints later phases must keep

1. Original evidence is never modified. Ever.
2. Everything produced from evidence is a derived asset or a run record with its own
   digest and a complete provenance chain.
3. Machine output is never self-certified; a human verifies before it becomes a finding.
4. A run that did not finish is never presented as one that did.
5. The audit trail is append-only.
6. Nothing is deleted to make a result look cleaner — a rejected detection stays.
7. TRACE reports observations. It does not draw forensic conclusions.
