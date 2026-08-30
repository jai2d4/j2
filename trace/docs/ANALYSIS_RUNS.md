# Analysis runs

Every detection in TRACE belongs to a run, and the run is what makes the detection
answerable. This document describes the record, its lifecycle, and the rules that keep it
honest.

---

## Why runs exist

A detection on its own is an assertion. A detection joined to its run can answer:

- which evidence item, and what were its bytes at the time (`evidence_sha256`)
- which model file, exactly (`model_sha256`)
- which provider and runtime version
- which device did the work
- what settings were in force
- whether the run finished, and if not, why
- when it happened and who started it

Storing this once per run rather than once per detection means hundreds of thousands of
rows cannot disagree with each other about their own provenance.

---

## The record

`analysis_runs` (migration `0003_analysis_and_detections.sql`):

| Column | Meaning |
|---|---|
| `id` | UUID |
| `case_id`, `evidence_id` | what was analysed |
| `analysis_type` | `object_detection` today; the extension point for later capabilities |
| `provider_name`, `provider_version`, `runtime` | what performed it |
| `model_name`, `model_version` | which model |
| `model_sha256` | **digest of the model file that actually ran** |
| `model_relpath` | where it was loaded from |
| `device_requested`, `device_used` | asked for vs. what ran |
| `configuration_json` | thresholds, sampling, class filter |
| `sampling_interval_us` | minimum spacing between analysed frames; 0 = every frame |
| `status` | see below |
| `started_at`, `completed_at` | ISO-8601 UTC |
| `frames_analyzed`, `frames_expected` | progress and estimate |
| `detections_stored` | rows written |
| `analyzed_duration_us` | how much media was covered |
| `source_width`, `source_height` | geometry at analysis time |
| `evidence_sha256` | the evidence digest when the run began |
| `error_message`, `warnings_json` | why it stopped; anything degraded |
| `created_by`, `created_at` | operator and time |

`sampling_interval_us` is a column rather than a JSON field because the viewer needs it:
it decides which analysed frame belongs to the playhead. Inferring that from the spacing
of the detections would put boxes on the wrong moment whenever a stretch of footage
contained nothing.

---

## Status

```
Queued ─► Running ─┬─► Completed    finished, covered what it was asked to cover
                   ├─► Cancelled    the operator stopped it
                   ├─► Partial      it stopped itself part-way and kept what it had
                   └─► Failed       it could not produce usable results
```

Two functions in `core/models/analysis_run.h` carry the meaning:

```cpp
bool isTerminal(AnalysisRunStatus status);              // will not change again
bool producedCompleteResults(AnalysisRunStatus status); // only Completed
```

The rules, enforced in `AnalysisRepository::finishRun`:

- **A run that carries an error is never recorded as `completed`.** The repository
  refuses the combination.
- **A non-terminal status is never written as a terminal one.**
- Cancelled and Partial are distinct and both honest: *the operator stopped this* is not
  the same as *this stopped itself*.
- A run that did not complete is labelled wherever it is shown, and the analysis panel
  adds: "These results do not cover the whole recording."

`latestUsableRun()` returns the most recent `completed` run, falling back to `partial` —
never a failed or cancelled one — and the panel makes that the run being viewed.

---

## Lifecycle

```
AnalysisPanel                AnalysisPipeline               AnalysisService
────────────                 ────────────────               ───────────────
Analyze ────────────────────► validate model
                             hash the model file
                             ─────────────────────────────► recordModelLoaded()
                             ─────────────────────────────► startRun()  → Running
                             open media, probe geometry
                             ─────────────────────────────► recordDevice()
                             per frame:
                               sample by timestamp
                               provider.analyze()
                               buffer
                               every 500 ──────────────────► storeDetections()
                               progress callback
                             ─────────────────────────────► finishRun(terminal status)
   ◄──── analysisFinished(succeeded, message, status)
```

The model is hashed **before** the run starts, and the digest that is recorded is the
digest of the file that will actually be loaded — not a value copied from the catalogue.
If they disagree, the run does not start: `ModelValidationFailed` is audited and the
operator is told what was expected and what was found.

Detections are written in batches of 500 inside one transaction each, reusing a single
prepared statement. An hour of footage produces hundreds of thousands of rows and a
transaction per row would dominate the run time.

---

## What a run may write

Only `analysis_runs`, `detections`, and the audit trail. The evidence file is opened for
reading and nothing else. The evidence record's `sha256` is never touched by analysis —
an analysis is not an integrity check, and the acceptance tests hash the managed original
before and after a run and require the bytes to be identical.

---

## Audit events

| Event | When |
|---|---|
| `analysis.model_loaded` | a model artefact was validated and loaded, with its digest |
| `analysis.model_validation_failed` | the artefact was missing, unreadable or the wrong file |
| `analysis.started` | a run was created |
| `analysis.completed` | a run finished with complete results |
| `analysis.cancelled` | the operator stopped it |
| `analysis.failed` | it could not produce usable results |
| `detection.confirmed` / `.rejected` / `.marked_uncertain` | an analyst's review decision |

The audit trail is append-only and enforced by database triggers, so none of this can be
rewritten by a later defect in the UI.

---

## Human review

Every detection starts `unreviewed`. TRACE never promotes its own output.

| State | Meaning |
|---|---|
| `unreviewed` | nobody has looked |
| `confirmed` | an analyst agrees |
| `rejected` | an analyst disagrees |
| `uncertain` | an analyst could not decide from this footage |

A rejected detection is **kept**. The filter can hide it, the overlay draws it dashed,
and the panel says "Rejected detections are hidden, not deleted." Deleting it would erase
the fact that the model reported it, which is part of the analytical history — and there
is no delete action in the interface at all.

Each decision records who made it and when, carries the analyst's optional note, and is
written to the audit trail.

### Reviewing at scale

An hour of footage produces tens of thousands of boxes. An analyst who has to click each
one either does not finish or stops looking properly somewhere around the four hundredth,
and both outcomes are worse than the alternative, so TRACE offers two ways through a run.

**From the keyboard.** With the detections panel focused:

| Key | |
|---|---|
| `C` | confirm |
| `X` | reject |
| `U` | uncertain |
| `Backspace` | clear the review |
| `J` / `↓` | next detection |
| `K` / `↑` | previous detection |
| `N` | next **unreviewed** detection |

Single keys, not modifier combinations: this is done thousands of times in a sitting.
**Advance after each decision** is on by default, so ruling on a detection moves to the
next unreviewed one — without it, reviewing is click, decide, click, decide, and the
clicking is most of the work. `N` is what makes a second pass through a part-reviewed run
practical.

The progress indicator is counted from the database on every refresh, not tracked as the
operator works, so it is still right after a restart and when two people review the same
run.

**In bulk.** *Mark all matching…* applies one decision to every detection the current
filter selects — a class group, a confidence floor, a time range, or any combination. The
confirmation names the count and the filter in words before anything happens.

### Why a sweep is recorded differently

A sweep and an examination reach the same verification state by very different means, and
the case file has to be able to tell them apart. "Confirmed" set by somebody looking at
this box and "confirmed" set by somebody sweeping two thousand boxes in a time range are
not the same claim about what a human examined.

So:

- **Each detection carries `review_method`** — `individual`, `bulk`, or nothing when
  unreviewed. It survives into the detection inspector, the report table ("part of a bulk
  decision"), and the JSON in an exhibit bundle.
- **A sweep writes one audit record**, naming the filter and the count, with `"bulk": true`
  and the word "Bulk review" in the description itself. Ten thousand identical records
  would describe ten thousand examinations that did not happen — to anyone auditing the
  case later it would read exactly like diligent individual review.
- **The progress indicator distinguishes them**: "8,000 of 8,000 reviewed · 12
  individually, 7,988 in bulk" describes a very different amount of human attention from
  "8,000 of 8,000 reviewed", and only one of them is true.
- **A sweep overwrites individual reviews inside its filter**, and marks them `bulk`. As of
  the sweep, the state is the sweep's doing; recording otherwise would credit the current
  value to an examination that did not produce it.

Rows reviewed before this existed have no `review_method`, and are **not** backfilled as
`individual`. Every one of them was in fact reviewed one at a time, but writing that in
would be asserting it from inference rather than from record, and the column exists
precisely so nobody has to infer it.

Nothing about bulk review deletes anything. Rejecting ten thousand detections marks ten
thousand rows rejected, exactly as rejecting one does.

A bulk review must name the evidence, the run or the case it applies to. Nothing in the
interface can produce a filter without one, which is why the service refuses it rather
than trusting it not to arrive — an unscoped sweep would rule on every detection in the
database.

---

## Multiple runs

An evidence item can be analysed many times — different models, thresholds, sampling.
Every run is kept and listed with its status, model, device and counts. The workspace
always shows the results of **one identified run**, so a box on the video can always name
the model that produced it, and two runs' results are never mixed on the same frame.
