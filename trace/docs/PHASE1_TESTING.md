# Phase 1 testing

What is covered, how to run it, and — as importantly — what was *not* exercised in this
environment.

---

## Running

```bash
cmake -S trace -B trace/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DTRACE_WITH_ONNXRUNTIME=ON \
      -DTRACE_ONNXRUNTIME_ROOT=trace/third_party/onnxruntime
cmake --build trace/build -j"$(nproc)"
ctest --test-dir trace/build --output-on-failure
```

The GUI suites need a platform plugin. `QT_QPA_PLATFORM=offscreen` is set for them
automatically; `xvfb-run -a ctest …` also works and is what a real X server run looks
like.

Two artefacts are optional. The tests that need them **skip themselves and say why**
rather than passing quietly:

```bash
./trace/scripts/fetch_models.sh        # YOLOX-Tiny, digest-verified
./trace/scripts/fetch_test_media.sh    # the street clip used for real inference
```

---

## The suites

| Suite | Cases | What it covers |
|---|---|---|
| `trace_unit_tests` | 81 | domain logic, detection geometry, decode and NMS maths, sampling, catalogue, run-status rules |
| `trace_ui_unit_tests` | 6 | overlay geometry, hit-testing, colour semantics, the no-modification rule |
| `trace_integration_tests` | 35 | ingestion, persistence, integrity, media, and the whole detection pipeline against the real database |
| `trace_acceptance_test` | 1 | the Phase 0 §37 workflow through the real window |
| `trace_acceptance_phase1_test` | 1 | the Phase 1 §41–§43 workflows through the real window |
| **Total** | **124** | |

Phase 0's baseline was **80 cases**, re-run before any Phase 1 code was written and again
after. It is still 80 of the 124, all passing: Phase 1 added tests, it did not replace or
remove any.

---

## Phase 1 unit coverage (32 cases)

| Group | Cases | Asserts |
|---|---|---|
| `Letterbox` | 6 | scale and offset, top-left placement, pad value, tensor layout, the inverse transform, round-tripping a box |
| `NonMaximumSuppression` | 4 | IoU maths, class-aware suppression, the per-frame cap, empty input |
| `YoloxDecode` | 2 | a hand-computed synthetic `[1, 3549, 85]` tensor decodes to the boxes worked out by hand, on all three strides |
| `ModelManager` | 4 | catalogue lookup, absent model reported not fetched, digest mismatch blocks, config built from the descriptor |
| `FrameSampler` | 3 | sampling follows presentation timestamps, variable frame rate, "every frame" mode |
| `DetectionOverlay` | 6 | normalised → widget mapping under letterboxing, proportional at any size, smallest-box hit test, colour semantics, **the frame image is byte-identical after drawing**, overlay off draws nothing |
| `MockProvider` | 2 | determinism, refuses to run before initialisation |
| `ClassTaxonomy`, `NormalizedBox`, `DetectionVerificationRules`, `DetectionProviderRegistry`, `AnalysisRunStatusRules` | 5 | grouping, box validity and clamping, review-state transitions, factory registration, terminal-state rules |

---

## Phase 1 integration coverage (11 cases)

`DetectionPipelineTest`, against a real SQLite database and real decoded media:

| Case | What it proves |
|---|---|
| `StoresDetectionsWithFullProvenanceAndCompletesTheRun` | the run records provider, model, digest, device, evidence digest; detections join back to it |
| `SamplingRespectsTheRequestedQuality` | analysed timestamps are at least one interval apart, taken from the media clock |
| `ConfidenceThresholdRemovesWeakDetections` | below-threshold detections are not stored |
| `CancellationNeverReportsCompletion` | a cancelled run is `cancelled`, keeps what it stored, and is never `completed` |
| `AnInvalidModelFailsTheRunWithoutTouchingEvidence` | digest mismatch blocks; evidence bytes unchanged |
| `AMissingModelIsReportedRatherThanDownloaded` | absent model → a message naming the path, no network access |
| `UnknownProviderIsRejectedBeforeAnyRunIsCreated` | no orphan run row |
| `ResultsAndVerificationSurviveARestart` | detections and review states reload from disk |
| `RejectedDetectionsAreKeptAndAudited` | rejection stores and audits, and deletes nothing |
| `OverlayQueryReturnsOneAnalysedFrameAtATime` | the overlay never mixes two sampled frames |
| `RealModelDetectsPeopleAndVehiclesInTestFootage` | **real inference**: people > 0, vehicles > 0, every box inside the frame, confidences in range, recorded `model_sha256` equals the on-disk digest, evidence still verifies |

---

## Phase 1 acceptance coverage

`trace_acceptance_phase1_test` drives the real `ApplicationContext`, `MainWindow`,
analysis panel, detections table, overlay and timeline.

**§41 — `AnalyzeVideoProducesReviewableResultsThatSurviveARestart`**
Configure provider/model/sampling/threshold in the real controls → Analyze → run recorded
`completed` with full provenance → managed original still byte-identical → run listed in
the history → detections listed → People and Vehicles lanes on the timeline → activating
a row jumps the playhead → overlay draws that frame's boxes, all inside the frame →
inspector shows the selected detection's provenance → group filter narrows and restores
without deleting → confirm one, reject another → both stored, rejected one kept →
audit contains started/completed/confirmed/rejected → **restart** → run, detections and
both review decisions come back → evidence still verifies against its ingestion digest.

**§42 — `AFailedAnalysisIsRecordedAsFailedAndNeverAsComplete`**
Select a catalogued model that is genuinely not installed → the run is `failed` with an
error message and zero detections, never `completed` → the operator is shown a dialog →
nothing is listed, no overlay is offered → evidence bytes unchanged → the audit has a
failure and no completion.

**§43 — `CancellingKeepsWhatWasFoundAndRecordsACancelledRun`**
Start a run and press the real Cancel button → the run ends `cancelled`, never
`completed` → `producedCompleteResults()` is false → what was already stored is still
there → evidence bytes unchanged → the audit has a cancellation and no completion → and
TRACE does *not* report the operator's own decision back to them as an error.

**`RealModelResultsAppearInTheWorkspace`** repeats §41 with ONNX Runtime and YOLOX-Tiny
on real street footage, and skips with a message when either artefact is missing.

---

## Two defects this suite caught

Both were found by the acceptance test and fixed, not worked around:

1. **The Cancel button was disabled for the whole run.** `analysisRunning()` was derived
   from the worker thread's own flag, and the controls were refreshed *before* the thread
   was started — so the operator could never stop an analysis. Now the panel owns an
   explicit in-flight flag set the moment a run is committed to.
2. **A crash on shutdown.** A queued `mediaOpened` signal reached the new detection panels
   after `ApplicationContext::shutdown()` had released the services, dereferencing a null
   `AnalysisService`. The Phase 0 guard (`isInitialised()`) is now applied to every Phase 1
   panel entry point that reads through a service.

---

## What was *not* tested here

State this plainly, because a claim about untested hardware is worth nothing:

| Not exercised | Why |
|---|---|
| **CUDA execution provider** | this environment has no NVIDIA GPU (`nvidia-smi` finds no device) and the CPU package of ONNX Runtime is installed. The CUDA path is compiled, guarded and selected from the runtime's own provider list, but **it has never run here** |
| **TensorRT** | not implemented in Phase 1 at all |
| **GPU performance** | no GPU, so no GPU figure is quoted anywhere in this repository |
| **Windows / macOS** | development and CI ran on Linux; the build is portable but was not executed elsewhere in this phase |
| **Hour-long recordings** | the longest clip tested is 795 frames (≈ 79 s of media) |
| **4K footage** | the largest tested is 768 × 576 |

The measured numbers in `docs/DETECTION_MODEL.md` are from this machine: Intel Xeon
@ 2.30 GHz, 4 vCPU, 15 GB RAM, no GPU.

---

## Determinism

The mock provider exists so the pipeline, database and UI can be tested on any machine
with no model, no GPU and no network. Its output is a fixed function of the frame
timestamp:

| Emitted | When | Confidence |
|---|---|---|
| `person` | every analysed frame | 0.90 |
| `car` | analysed frames on an even whole second | 0.75 |
| `dog` | every analysed frame | 0.30 |

It reports no model digest, because there is no artefact. It is not a detector and is
never presented as one.
