# Legacy grading

The repository audit found no `src/lib/engine.ts` or generic fantasy grading
implementation to move. Existing production services remain untouched here so
the current TruGrade interface and API continue to work. If that legacy engine
is recovered later, place it in this directory rather than using it for official
film grades.
