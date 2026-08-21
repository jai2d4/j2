# Provenance

Every analytical statement TRACE ever makes must be traceable back to the evidence that
produced it. Phase 0 does not analyse anything, but it builds the chain that later
phases record into, so that no future module has to invent one.

## The chain

```
CASE
 └─ ORIGINAL EVIDENCE            immutable, hashed at ingestion
     └─ SOURCE TIMESTAMP/FRAME   where in the media this came from
         └─ PROCESSING OPERATION what was run
             ├─ PARAMETERS       exactly how it was run
             ├─ SOFTWARE VERSION which build, which libraries
             └─ MODEL + VERSION  which model, when there is one
                 └─ DERIVED ASSET  the output, with its own digest
                     └─ ANALYST VERIFICATION  human accepted, rejected, or not reviewed
                         └─ REPORT / EXHIBIT
```

## Where it lives

`processing_operations` records the operation:

| Column | Answers |
|---|---|
| `evidence_id` | What source evidence produced this? |
| `source_start_us`, `source_end_us`, `source_frame_number` | What timestamp or frame produced it? |
| `operation_type` | What operation was performed? |
| `software_name`, `software_version` | What software performed it? |
| `library_versions` | Which decoder/codec builds were in effect (JSON) |
| `model_name`, `model_version` | Which model, for future analysis providers |
| `parameters_json` | What parameters were used? |
| `started_at`, `completed_at`, `status`, `error_message` | When, and did it succeed? |
| `performed_by` | Who performed it? |

`derived_assets` records the output: `evidence_id`, `operation_id`, `parent_asset_id`
(derivations of derivations), `storage_relpath`, `sha256`, the same source-position
columns, and `verification_state`.

## Rules

1. **One write path.** Everything derived from evidence goes through
   `DerivedAssetService::registerAsset`, which hashes the file, writes the operation and
   the asset in one transaction, and emits the audit record. Nothing writes
   `derived_assets` directly.
2. **Machine output is never self-certified.** `verification_state` starts at
   `unverified`. Only a person moves it to `human_verified` or `rejected`; TRACE never
   marks its own output as confirmed.
3. **Originals are never inputs to themselves.** A derived asset always names the
   original it came from, and the original is opened read-only.
4. **An operation that fails still gets a record**, with `status` and `error_message`, so
   an absent result is explainable.

## Worked example — "Save current frame"

```json
{
  "operation_type": "frame_extraction",
  "software_name": "TRACE", "software_version": "0.1.0",
  "library_versions": { "libavformat": "60.16.100", "libavcodec": "60.31.102", … },
  "source_start_us": 5000000,
  "source_frame_number": 125,
  "parameters": {
    "method": "libavcodec decode to RGB24, lossless PNG encode",
    "source_timecode": "00:00:05.000",
    "width": 320, "height": 240, "scaled": false,
    "source_codec": "h264", "source_pixel_format": "yuv420p",
    "frame_rate_mode": "constant_suspected",
    "key_frame": false
  },
  "performed_by": "analyst1"
}
```

The derived asset row carries the PNG's own SHA-256; the audit record carries the same
identifiers. From the exported image alone you can reach the operation, the source
evidence, its digest, and the exact moment in the recording it came from.

## What a Phase 1+ analysis module must do

An `IAnalysisProvider` implementation returns an `AnalysisResult` carrying provider name
and version, model name and version, parameters, runtime information and a list of
findings — each with its own source timestamp, optional range, optional frame number and
provider-reported confidence.

Persisting those results means writing one `processing_operations` row and one
`derived_assets` row per output artefact. No schema change is required for detections,
transcripts or embeddings: they are derived assets of type `analysis_output` whose
payload lives in the case's `working/` directory, and they inherit hashing, provenance
and audit for free.

**Findings are claims, not conclusions.** They stay `unverified` until an analyst says
otherwise, and TRACE's language must never present a model output as a determination.
