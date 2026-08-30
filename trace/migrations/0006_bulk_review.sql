-- Migration 0006 — recording *how* a detection was reviewed.
--
-- Reviewing detections one at a time does not survive contact with a real run:
-- an hour of footage produces tens of thousands of boxes, and an analyst who
-- has to click each one will either not finish or stop looking properly around
-- the four hundredth. So TRACE gains a way to apply one decision to everything
-- matching a filter.
--
-- That capability creates an obligation. "Confirmed" set by someone who looked
-- at this box and "confirmed" set by someone who swept two thousand boxes in a
-- time range are not the same claim, and a report that presented them
-- identically would overstate what a human actually examined. This column is
-- what keeps them distinguishable, in the database and therefore in anything
-- derived from it.
--
-- NULL means the detection has not been reviewed. Existing rows keep their
-- verification state and get NULL here rather than being guessed at: every
-- review recorded before this migration was made one at a time, but backfilling
-- 'individual' would be asserting that from inference rather than from record,
-- and this column exists precisely so that nobody has to infer it.

ALTER TABLE detections ADD COLUMN review_method TEXT;

-- Reporting and the review-progress indicator both count by state within a run,
-- and the existing index is on (evidence_id, verification_state).
CREATE INDEX IF NOT EXISTS idx_detections_run_verification
    ON detections(analysis_run_id, verification_state);
