# AI architecture

How analysis is bolted onto the evidence foundation, and why it is bolted on rather
than built in.

---

## The shape of it

```
              ┌──────────────────────────────────────────────┐
   operator → │ ui/analysis   AnalysisPanel · DetectionsPanel │
              │               DetectionInspector · overlay    │
              └───────────────┬──────────────────────────────┘
                              │  DetectionAnalysisRequest
              ┌───────────────▼──────────────────────────────┐
              │ analysis/     AnalysisPipeline · FrameSampler │
              └────┬─────────────────────┬───────────────┬───┘
                   │                     │               │
        ┌──────────▼──────┐   ┌──────────▼────────┐  ┌───▼───────────────┐
        │ media/          │   │ ai/detection      │  │ core/services     │
        │ VideoDecoder    │   │ IDetectionProvider│  │ AnalysisService   │
        │ (FFmpeg)        │   │ ModelManager      │  │ AuditService      │
        └─────────────────┘   └───────────────────┘  └───────────────────┘
```

`analysis/` is the only module that knows about all three. It contains no Qt, so the
whole detection path is testable headlessly, and no SQL, so storage policy stays in
`core/services`.

The dependency direction the build enforces:

```
apps/desktop → ui → { core, media, ai, analysis }
                       analysis → { core, media, ai }
                       media    → core
                       ai       → core
```

---

## The extension point

`ai/detection/detection_provider.h` is the whole coupling surface:

```cpp
class IDetectionProvider {
public:
    virtual ProviderInfo info() const = 0;
    virtual DetectionCapabilities capabilities() const = 0;
    virtual Status initialise(const DetectionProviderConfig& config) = 0;
    virtual bool isReady() const = 0;
    virtual Result<DetectionBatchResult> analyze(const FrameInput& frame,
                                                 const DetectionOptions& options) = 0;
    virtual void shutdown() {}
};
```

A provider is handed one decoded RGB frame with its real presentation timestamp, and
returns observations in **source-frame normalised coordinates** — it undoes its own
letterboxing before returning, so nothing downstream needs to know how the model was
fed.

Nothing above this interface names a runtime, a vendor or a model family. There is no
`#include` of ONNX Runtime, CUDA, TensorRT or any cloud SDK outside
`ai/detection/providers/`. Swapping runtimes is adding a class:

```cpp
class TensorRtDetectionProvider : public IDetectionProvider { … };

RegisteredDetectionProvider entry;
entry.id = "tensorrt";
entry.displayName = "TensorRT (local GPU)";
entry.factory = [] { return std::make_shared<TensorRtDetectionProvider>(); };
DetectionProviderRegistry::instance().registerProvider(std::move(entry));
```

The evidence model, storage, timeline, overlay, audit trail and database schema do not
change.

### What ships in Phase 1

| Provider id | What it is | Model artefact |
|---|---|---|
| `onnxruntime` | ONNX Runtime 1.17.3, CPU execution provider, CUDA attempted first when the build has it | required |
| `mock` | Deterministic fixed output, no model, no device | none |

The registry holds **factories**, not instances: listing what is installed loads no
model and acquires no device. That only happens when a run starts.

The mock provider exists so the database, pipeline and interface can be exercised on any
machine — no GPU, no model, no network. It says "mock" in its provider name and reports
no model digest, because there is no artefact. It is never offered as a detector.

---

## Threading

```
GUI thread          BackgroundTask worker            decoder thread
────────────        ─────────────────────            ──────────────
Analyze ─────────►  AnalysisPipeline::execute
                      ├─ spawns ─────────────────►   decode → bounded queue (4)
                      ├─ pops frame  ◄───────────────────────┘
                      ├─ provider.analyze()
                      ├─ buffer → batch of 500 → one transaction
                      └─ progress callback (every frame)
   ◄── 150 ms timer reads the last progress under a mutex
```

- The GUI thread never decodes, never runs a model and never writes a detection row.
- The frame queue is bounded at four frames, so a 4K recording cannot accumulate
  decoded frames in memory: inference sets the pace and the decoder blocks.
- The pipeline calls the progress callback **once per analysed frame** and does not
  throttle. Throttling is a presentation concern: `AnalysisPanel` stores the latest
  progress under a mutex and repaints on a 150 ms timer.
- Cancellation is checked every frame, from two sources: the callback's return value and
  a `shared_ptr<std::atomic<bool>>` carried on the request. The second exists so the
  Cancel button works without waiting for the next progress update.

---

## What analysis is allowed to touch

Analysis **reads** the managed original and **writes** rows to `analysis_runs` and
`detections`. That is the entire write surface.

It does not, and cannot through this path:

- modify, re-encode or overwrite the evidence file
- burn boxes into any video
- replace or recompute the evidence's recorded SHA-256
- delete a detection, including a rejected one

The overlay is drawn onto the widget in `paintEvent`, never into the decoded `QImage` —
there is a unit test (`DetectionOverlay.DrawingBoxesLeavesTheFrameImageUntouched`) that
compares the frame's bytes before and after.

---

## What Phase 1 deliberately does not do

Object detection reports a **visual class** and a **confidence**. It does not:

- recognise faces or compute biometric templates
- name a person, or link an observation to an identity
- read number plates
- track an object across frames, or re-identify one across cameras
- designate a suspect, classify a weapon, or reconstruct an event
- answer natural-language questions about footage
- run against a live camera feed or query any external database

These are not "not finished". They are out of scope for this phase, and several of them
are out of scope for this software.

---

## Where the next module plugs in

A second analysis type (audio events, scene classification) follows the same shape:

1. a provider interface in `ai/<capability>/`
2. a pipeline in `analysis/` that joins decode → provider → service
3. an `analysis_type` value on `analysis_runs` (`object_detection` is the only one today)
4. its own results table, keyed to `analysis_run_id`
5. a panel in `ui/`, and a timeline lane pushed through the existing
   `TimelineWidget::setTracks`

The timeline widget already knows nothing about detections: it draws whatever tracks it
is given.
