# REVIEW.md

How changes to Sineless are reviewed: by the `reviewer` subagent before a push, and by the
`ai-review` check on every pull request. Both read this file; read CLAUDE.md and the
`.claude/rules` file of each folder the change touches too.

## Questions

Answer each, citing file and line:

1. **Same cause elsewhere:** does the cause this change fixes also exist, unfixed, in another
   display mode, time base, live or frozen state, channel count, readout or platform?
2. **Values:** does the change merge, equalise or derive values, or change a threshold,
   floor, tolerance or margin? Is each value's meaning, purpose and margin stated and true?
3. **Tests:** does every fix have a test that fails without it? Does every new failure branch
   have a test that triggers it? Would the test catch the bug at the boundaries (highest
   sample rate, shortest span, failed read)?
4. **Simpler or documented way:** for platform, SDK or standard code, is there a documented
   or widely adopted approach the change ignores?
5. **Regressions:** can the change break a case that worked before: another mode, a host,
   real-time safety, performance on steady input?

## Severity

A finding is **Important** when it would let a defect or a regression reach a user, or breaks
a rule the product relies on:

- a correctness bug or regression, with a concrete scenario that triggers it;
- anything on the audio thread that can allocate, lock, block or do I/O;
- a fix without a test that fails without it, or a new failure branch with no test;
- a break of "Rules the product relies on" in CLAUDE.md.

Everything else is a **nit**. Report at most three nits, and none once the pull request has
had a review: after the first round, report Important findings only, so that reviews
converge.

## Every finding

- cites the file and line;
- gives a concrete scenario: the input or state, and what goes wrong;
- is checked against the code before it is reported. Drop a finding you cannot back with the
  code; do not report style or ask for extra process.

The author fixes the findings they agree with and answers each one they reject, saying why.
