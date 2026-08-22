# Model provenance

The chain that connects a box on screen to the exact bytes of the model that drew it.

---

## The question this answers

> *"A box in your report says 'person, 88%'. What produced it?"*

TRACE can answer, from the database alone, without trusting anything in the interface:

```
detection.id
  → detection.analysis_run_id
      → analysis_runs.model_name, model_version
      → analysis_runs.model_sha256      the digest of the file that ran
      → analysis_runs.provider_name, provider_version, runtime
      → analysis_runs.device_used
      → analysis_runs.configuration_json
      → analysis_runs.evidence_sha256   the evidence bytes at the time
      → analysis_runs.status            did it finish?
  → detection.timestamp_us              the analysed frame, from the media clock
  → detection.bbox_x/y/w/h              where, normalised to the source frame
  → detection.verification_state        what a human said about it
```

Every one of those is shown in the **Detection** inspector, and every one is a stored
column — not a value recomputed for display.

---

## Why the digest, and not the name

A model file is an artefact like any other piece of evidence-adjacent material. Two files
called `yolox_tiny.onnx` are not necessarily the same file: one may be fine-tuned,
truncated, quantised or corrupt. A name proves nothing.

So:

1. `ModelManager::validate()` hashes the file **at the path that will actually be
   loaded**, immediately before the run.
2. That digest — not the catalogue's expected value — is what gets recorded on the run.
3. When the catalogue *does* record an expected digest, a mismatch **blocks the run**:
   `ModelValidationFailed` is audited and the operator is shown the expected and actual
   values.

```cpp
struct ModelValidation {
    bool present = false;
    bool checksumMatches = false;   // true when no expected digest was recorded
    std::string sha256;             // the digest of the file that would run
    std::int64_t fileSize = 0;
    std::filesystem::path path;
    std::string problem;            // empty when usable
    bool usable() const { return present && checksumMatches && problem.empty(); }
};
```

The analysis panel shows the same digest before a run starts, so the operator can see
which artefact is selected without starting anything.

---

## Where models live

| | |
|---|---|
| Default | `<data root>/models/` |
| Override | `TRACE_MODEL_DIR` environment variable |
| Development | `trace/models/` (git-ignored) |

Models are never committed. They are large, they carry their own licence, and a digest in
the catalogue is a better record than a binary in git history.

---

## TRACE does not download models

There is no code path in the application that fetches a model. An absent model is
reported as absent:

> The model file was not found at `/…/models/yolox_s.onnx`. Install it with
> `scripts/fetch_models.sh`, or choose another provider.

and the analysis panel adds, in the model status line:

> TRACE never downloads a model on its own.

Fetching is an explicit operator action through `scripts/fetch_models.sh`, which prints
the source URL and the licence, verifies the SHA-256 against the catalogue, and installs
nothing on a mismatch.

This matters in a deployment: an agency workstation may have no outbound network at all,
and software that quietly reaches for a model would be both a surprise and, on some
networks, a policy breach.

---

## The catalogue

`ai/detection/models/model_manager.cpp` holds one `ModelDescriptor` per model TRACE knows
how to drive:

| Field | Purpose |
|---|---|
| `id`, `name`, `version` | identity |
| `family` | how to decode the raw output (`yolox`) |
| `fileName` | what to look for in the model directory |
| `expectedSha256` | catalogue digest; a mismatch blocks the run |
| `inputWidth`, `inputHeight` | model input geometry |
| `inputIsBgr`, `inputScaledTo01`, `padValue` | the preprocessing the model was trained with |
| `performsOwnNms` | whether the graph already applies NMS |
| `strides` | grid strides for grid-decoded families |
| `classes` | the label list |
| `licence`, `source`, `notes` | where it came from and under what terms |

The preprocessing fields are properties of the *artefact*. Putting them in the descriptor
rather than in the runtime is what lets one provider drive several models without
special-casing any of them by name.

---

## Licences of what Phase 1 pulls in

| Component | Licence | Committed? |
|---|---|---|
| ONNX Runtime 1.17.3 | MIT | no — `scripts/fetch_onnxruntime.sh` |
| YOLOX-Tiny 0.1.1rc0 | Apache-2.0 | no — `scripts/fetch_models.sh` |
| OpenCV `vtest.avi` (test clip) | BSD-3-Clause | no — `scripts/fetch_test_media.sh` |

`.gitignore` covers `third_party/onnxruntime/`, `models/`, `*.onnx`, `*.trt`, `*.engine`
and `tests/fixtures/external/`, so none of it can be committed by accident.

---

## What is *not* claimed

The chain above establishes **what produced a detection**. It does not establish that the
detection is correct.

A confidence score is the model's own score for a visual class. It is not a probability
that the object is what the label says, and it is not evidence of anything by itself.
That is why every detection starts `unreviewed`, why the review states are stored
separately from the model's output, and why the detection inspector carries this line:

> TRACE reports a visual class and the model's confidence in it. It does not identify
> individuals, read number plates, or draw conclusions about what was happening.
> Interpretation is the analyst's.
