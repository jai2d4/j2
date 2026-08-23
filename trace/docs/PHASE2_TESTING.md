# Phase 2 testing

What is covered, and what was not exercised.

---

## The suites

| Suite | Cases | Phase 2 additions |
|---|---|---|
| `trace_unit_tests` | 81 | — |
| `trace_ui_unit_tests` | 6 | — |
| `trace_integration_tests` | 46 | **11** (`ReportExportTest`) |
| `trace_acceptance_test` | 1 | — |
| `trace_acceptance_phase1_test` | 1 | — |
| `trace_acceptance_phase2_test` | 1 | **3 workflows** |
| **Total** | **136** | |

Phase 0's 80 and Phase 1's 44 are unchanged and all still pass.

## Integration coverage (11 cases)

| Case | What it proves |
|---|---|
| `ExportsABundleWhoseEveryFileMatchesItsRecordedDigest` | Every file in the manifest re-hashes correctly; the layout is complete; the managed original is unchanged |
| `TheReportCarriesTheLimitationsStatementAndNoConclusion` | The limitations statement is present; banned wording is absent; signing is mentioned exactly once and only as a denial |
| `ChangingAnExhibitByOneByteFailsVerification` | Tampering is caught, and the failing file is named |
| `EditingTheManifestIsCaughtByItsOwnDigest` | An altered `MANIFEST.checksums` fails against `MANIFEST.sha256`, and the per-file results are not reported as passes |
| `AFilePlantedInTheBundleIsReportedAsUnlisted` | A file nothing vouches for fails verification |
| `AFailedExportIsNeverRecordedAsExported` | A failed export is recorded failed, with an error and no manifest digest |
| `TheRepositoryRefusesToRecordAnExportedRunThatCarriesAnError` | The rule holds at the storage boundary: `exported` + error, a non-terminal status, and `exported` without a bundle are all refused |
| `CancellingLeavesNoBundlePresentedAsComplete` | No partial bundle survives a cancellation |
| `CitingAnUnconfirmedDetectionWithoutOptingInFailsTheExport` | The confirmed-only default cannot be bypassed by a caller |
| `OptingInSaysSoInTheReportItself` | A report containing unreviewed detections carries the notice and shows each review state |
| `ReportsAndTheirBundlesSurviveARestart` | The report, its items and its bundle reload and re-verify |

## Acceptance coverage

Driven through the real `MainWindow` and `ReportBuilderDialog`.

**§1 `ExportsABundleThatVerifiesIndependentlyAndSurvivesARestart`** — open the builder,
name the report, select evidence, export → recorded exported and self-verified → PDF
produced by the Qt renderer → **every file re-hashed by the test itself, reading
`MANIFEST.checksums` directly and never calling TRACE's verifier** → both manifests
covered by `MANIFEST.sha256` → `VERIFY.md` carries the commands and the
not-a-signature statement → managed original unchanged → audit has created and exported
and no failure → **restart** → the report reloads as exported with the same manifest
digest, and the bundle still verifies.

**§2 `AFailedExportIsRecordedFailedAndLeavesNoBundle`** — export into a path that cannot
be created → recorded failed with an error → no bundle on disk → the operator is shown a
dialog → audit has a failure and no export.

**§3 `CancellingLeavesNothingPresentedAsComplete`** — Cancel is enabled the instant the
export commits (the guard against the Phase 1 defect where a Cancel button stayed
disabled for a whole run) → cancelling leaves no bundle and nothing recorded as exported.

## Two defects the tests caught

Both were in the tests, not the code, and both were corrected rather than worked around:

1. A banned-substring check for `"digitally signed"` failed on the report's own
   disclaimer, *"Nothing here is digitally signed"*. The check now requires exactly one
   mention and requires it to be a denial — which is a stronger assertion than the
   original.
2. A test expected the display string `"Unreviewed"`; the codebase uses `"UNREVIEWED"`.

## What was not tested

| Not exercised | Why |
|---|---|
| **PDF content** | The test asserts a non-empty `REPORT.pdf` is produced by the Qt renderer. It does not parse the PDF or compare its layout to the HTML |
| **Large bundles** | The largest tested contains a handful of files; no hour-long clip or multi-gigabyte exhibit has been exported |
| **Clips from varied containers** | Only the synthetic H.264/MP4 sample; stream copy from other containers is untested |
| **Re-encode fallback** | Not implemented, so not tested |
| **Windows and macOS** | Not built or run in this phase; the PowerShell verification snippet in `VERIFY.md` is written but unexecuted |
| **A third party on a clean machine** | Verification is proven by re-hashing inside the test process. Nobody has yet taken a bundle to a machine without TRACE and followed `VERIFY.md` by hand |

That last row is the one to close first: the whole design rests on a bundle being
checkable by someone with nothing but their operating system, and that has been
demonstrated in code but not in practice.
