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
| GPU decode | Built, off by default. Enumeration and fallback are tested; the accelerated path has never run on a GPU. See `docs/HARDWARE_DECODE.md` |
| Multi-user deployments | `users` table, `UserRole`/`Permission` gate already enforced in services, including detection review |
| Reports and exhibits | `reports/` and `exports/` directories; the run record answers what a report must cite |
| Multi-camera synchronisation | Positions are real presentation timestamps throughout |

---

## Phase 3 — built

**Audio.** Decoding, the waveform as a derived asset and timeline track, playback
through `QAudioSink`, and the volume and mute controls. See `docs/AUDIO.md`.

One piece of it is **written but unproven**: no machine with an audio device has run
the sink. `docs/AUDIO.md` §5 lists exactly which claims are measured, which are not, and
what to check on real hardware — the most likely thing to need adjusting is whether a
given backend's `processedUSecs()` reports played-out or merely written audio.

Still open from this area:

- **Time-stretched audio.** Other playback speeds silence the track rather than
  pitch-shift it, because a pitch-shifted voice misrepresents a recording. Honest
  playback at 0.5× and 2× needs `libavfilter`'s `atempo`, which is not yet a dependency.
- **Audio events as an analysis type.** `analysis_runs.analysis_type` and the waveform
  pipeline are both in place; a detector would need its own results table and a
  pipeline in `analysis/`.

## Live camera capture — built

**Recording from a camera**, not just importing files from one. Network cameras over
Ethernet or WiFi (RTSP/RTMP/SRT/HTTP via avformat), attached cameras through
libavdevice, and Bluetooth as what it actually is: a control link that tells a camera to
bring up its WiFi. See `docs/CAMERA_INGEST.md`.

The part that mattered most was not the transport. A live camera has **no original**, so
TRACE is the recording device and the chain of custody starts with this process on this
clock. A captured segment therefore carries capture provenance — `"no_original_exists"`,
the camera, the transport, the machine clock — rather than the ingest record that would
imply the material came from somewhere. A dropped link ends the segment, records the gap
with its cause, and starts a new one; nothing is joined into a file that presents
continuous timestamps across missing time.

Two parts are **written but unproven**, for want of hardware:

- **Attached cameras.** libavdevice is linked and registered, but this machine has no
  `/dev/video*` device and the path has never opened one.
- ~~**The RTSP handshake specifically.**~~ — covered. The tests bring their own RTSP
  server: SDP from `av_sdp_create`, RTP from FFmpeg's own muxer, interleaved per RFC
  2326 §10.12, and assertions that TRACE negotiates DESCRIBE, SETUP and PLAY rather
  than merely producing a file. What remains uncovered is RTP-to-wall-clock timestamp
  establishment, because that server sends no RTCP sender reports.

  Building it found real behaviour worth fixing: a single packet with no timestamp —
  which is what an RTSP depacketiser delivers first — ended the entire capture and left
  an empty container. TRACE now places such a packet on the recording's own timeline and
  counts the substitution into the provenance record.

**Bluetooth video is not coming.** There is no Bluetooth video profile in general use and
BLE's throughput is one to two orders of magnitude short of compressed 1080p. The
platform backends (BlueZ, WinRT, CoreBluetooth) are three separate integrations and none
is implemented; every entry point refuses with the reason rather than returning an empty
list that reads as "no cameras nearby".

Still open from this area:

- **A Bluetooth backend**, on at least one platform, so `beginStreaming()` can do the
  BLE-to-WiFi handoff it is shaped for. Needs an adapter and a camera to test against.
- **Windows device enumeration.** libavdevice's DirectShow listing writes to the log
  rather than offering an API; `findLocalDevices()` says so instead of returning an empty
  list, and cameras are added by name there.
- **Scheduled and continuous capture** — a camera recorded on a schedule, or
  continuously with rolling retention, which is a different lifecycle from an operator
  pressing Record.

### Phase 4 — then, in rough order

- **Prove hardware decode on real hardware** — the accelerated path is written and has
  never executed. `hwaccel::verifyMatchesSoftware` and the
  `HardwareDecodeOnRealHardware` test exist to be run on a machine with a GPU; until
  somebody does, "written and unproven" is the accurate description and the setting stays
  off by default.

- ~~**Detection review at scale**~~ — built. Keyboard review with auto-advance, bulk
  confirm/reject across a filter, and a review-progress indicator per run. A sweep is
  recorded as a sweep rather than as N examinations; see `docs/ANALYSIS_RUNS.md`.
- **GPU verification** — the CUDA path is written and guarded but has never executed on a
  GPU here. What *was* fixed without one: TRACE no longer records `CUDA:0` merely because
  the runtime build offers the provider. It now reads the execution provider out of a
  profiled warm-up inference and records what actually ran, or `CUDA:0 (unconfirmed)`
  when the probe cannot tell. See `docs/PHASE1_TESTING.md`. The probe itself is written
  and unexecuted, because this build's ONNX Runtime is CPU-only. It needs a machine with an NVIDIA device, a GPU package of ONNX Runtime and a
  measured comparison before any GPU performance claim is made.
- **Working copies** — a proportionate transcode for awkward codecs, recorded as a
  derived asset with its parameters, so analysis runs on a known format while the
  original stays untouched.
- ~~**Local accounts**~~ — built. PBKDF2-HMAC-SHA256, lockout, an audited sign-in and
  a role gate with teeth. See `docs/AUTHENTICATION.md`, whose §6 lists what it
  deliberately does not protect.
- ~~**Encrypted storage**~~ — built. SQLCipher for the case database, AES-256-GCM
  containers for the evidence, and a keyring that lets several operators hold the same
  workspace key without sharing a password. See `docs/ENCRYPTION.md`, whose §6 lists
  what it does not protect.
- ~~**Encrypt derived assets**~~ — built. Thumbnails, waveforms, exported frames and
  clips go into the same containers under the same case key, through the one choke point
  every derived artefact already passed through. The recorded digest and size describe
  the plaintext, as they do for evidence; a locked workspace refuses to produce a derived
  asset rather than writing one in the clear; and exhibit bundles are decrypted on the
  way out, because a bundle exists to be checked with `sha256sum` by somebody who has
  neither TRACE nor the key. `docs/ENCRYPTION.md` §6.1 now says what is true instead of
  what was missing.

  Fixing it surfaced a second bug worth naming: **clip export had never learned to read
  encrypted evidence**. It opened the managed original by plain path, so in an encrypted
  workspace it met a container and reported "Invalid data found when processing input" —
  the opaque codec-shaped error that `EncryptedMediaIo` exists to prevent. It now goes
  through the decrypting IO layer like every other reader.
- **Windows encrypted builds** — SQLCipher and libcrypto are both in vcpkg and nothing
  in the code is platform-specific beyond the random source, but CI does not yet build
  Windows with `TRACE_WITH_ENCRYPTION=ON`, so that path is written and unproven.

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
