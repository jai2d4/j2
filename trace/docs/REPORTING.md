# Reports and exhibit bundles

How a case becomes something you can hand to someone else, and what that something is
allowed to say.

---

## The bundle

Exporting a report writes a self-contained directory:

```
CASE-0001_exhibit_2026-08-23T140000Z/
  MANIFEST.sha256         digests of the two manifests below
  MANIFEST.checksums      every content file, in sha256sum format
  MANIFEST.json           the same list, plus who exported it and when
  REPORT.html             the report
  REPORT.pdf              the same content, paginated (desktop application only)
  VERIFY.md               how to check all of this without TRACE
  exhibits/               the frames and clips the report cites
  provenance/
    evidence.json         every evidence item cited, with digests and ingestion detail
    analysis_runs.json    every run cited: provider, model, model digest, device, config
    detections.json       every detection cited, with its run and its review state
  audit/
    audit_extract.json    the audit events supporting all of the above
    audit_extract.csv     the same, for spreadsheets
```

Paths inside the manifests are relative, so a bundle survives being moved, copied or
renamed at its root.

## Three manifests, and why

Verification must not depend on TRACE, on a network, or on a scripting language the
reader may not have. So the file list is written twice:

| File | Purpose |
|---|---|
| `MANIFEST.checksums` | `sha256sum`'s own format. `sha256sum -c` consumes it directly. |
| `MANIFEST.json` | The same list plus export identity, for anything reading it programmatically. |
| `MANIFEST.sha256` | Digests of the other two, so tampering with a manifest is caught. |

Two commands check an entire bundle:

```bash
sha256sum -c MANIFEST.sha256
sha256sum -c MANIFEST.checksums
```

That is the whole verification story. It needs no parser, no Python, and no trust in the
software that produced the bundle.

## What a report contains

| Section | Content |
|---|---|
| Header | Case, investigator, agency, export time, operator, TRACE version, report id |
| Limitations | The fixed statement below, before anything else |
| Evidence | Each item: number, filename, ingestion time, size, integrity status, SHA-256 |
| Technical detail | Container, codec, resolution, duration, frame rate — where the file provided them |
| Observations | Each cited detection: time, class, confidence, review state, normalised box |
| Exhibits | Each frame and clip, with its digest and how it was produced |
| Analysis provenance | Per run: provider, runtime, model, **model SHA-256**, device, sampling, config, evidence digest at run time, run status |
| Bookmarks and notes | What the analyst marked |
| Chain of custody | The audit extract, oldest first |

## What a report may never say

The report presents observations and provenance. It states no conclusions, and these are
absent by construction, with a test that fails if any of them appear:

- any identification of a person — "person detected" is permitted, "the suspect" is not
- any claim a detection is correct, proven or verified-as-true
- any legal characterisation: "proves", "evidence of", "consistent with the offence"
- any claim of court-admissibility, certification or forensic soundness
- any claim that anything is signed or tamper-proof

Every report carries this, not editable from the interface:

> This document records what TRACE observed and what was done to the evidence it
> describes. Detections are machine-produced observations of a visual class with a
> confidence score; they are not identifications and are not conclusions. Where a
> detection is marked confirmed, that records only that the named analyst agreed with it
> at the time stated. Interpretation of this material is the analyst's and the court's.

### Confirmed detections only, by default

The report builder offers confirmed detections. Including unconfirmed ones is possible
but deliberate: the checkbox is off by default, the choice is stored on the report, and a
report that used it carries a prominent notice saying so and shows each detection's
review state.

The rule is enforced in the service, not only in the dialog: citing an unconfirmed
detection in a report that did not opt in **fails the export**.

## Clips

A clip is a **stream copy**. The encoded packets are remuxed into a new container without
being decoded and re-encoded, so the clip contains the original frames rather than a
generation-loss copy.

Two consequences are recorded rather than hidden:

- A stream copy cannot begin mid-frame, so extraction starts at the nearest preceding
  keyframe. Where that differs from the requested start, both are stored on the derived
  asset and stated in the report.
- Audio and video share one time origin, taken from the first video packet written. A
  per-stream origin would shift them by different amounts and desynchronise the clip.

Re-encoding is not implemented. A range that cannot be stream-copied is reported as such
rather than silently transcoded.

## What an export may write

`reports`, `report_items`, `derived_assets`, `processing_operations`, the audit trail,
and the bundle directory. The managed original is opened read-only. The acceptance and
integration tests hash it before and after every export and require it to be identical.

## Statuses

```
Draft ─► Exporting ─┬─► Exported    written and verified
                    ├─► Cancelled   the operator stopped it
                    └─► Failed      it could not be written
```

An export that carries an error is never recorded as `exported`, and a non-terminal
status is never written as a terminal one — both refused by the repository, so no caller
can bypass them. A failed or cancelled export removes its partial directory.

**An export verifies its own output before it is recorded as exported.** The service runs
the same verifier a third party runs, over the bundle it has just written. An export that
cannot verify what it wrote is a failure.

## Audit events

`report.created` · `report.exported` · `report.export_failed` · `report.export_cancelled`
· `clip.exported` · `report.bundle_verified` · `report.bundle_verification_failed`
