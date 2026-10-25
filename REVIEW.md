# REVIEW.md

How changes to Sineless are reviewed: by the `review` skill before a push, and by the
`ai-review` check on every pull request that is not a draft and not from a fork. Both run the
specialist reviewers in `.claude/agents` (correctness, rules, tests and, for C++ in `engine/`,
`plugin/` or `editor/`, real-time safety; `review-rules` alone for a change to documentation
only: Markdown, or files under `docs/`),
then a `review-verifier` on each Important finding, which keeps
it only if it can prove it from the code. Every reviewer reads this file; read CLAUDE.md and
the `.claude/rules` file of each folder the change touches too, and the parts of
`docs/DESIGN.md` that cover the behaviour it changes.

## Questions

Answer each, citing file and line:

1. **Same cause elsewhere:** does the cause this change fixes also exist, unfixed, in another
   display mode, time base, live or frozen state, channel count, readout or platform?
2. **Values:** does the change merge, equalise or derive values, or change a threshold,
   floor, tolerance or margin? Is each value's meaning, purpose and margin stated and true?
3. **Tests:** does every fix have a test that fails without it? Does every new failure branch
   have a test that triggers it? Would the test catch the bug at the boundaries (highest
   sample rate, shortest span, failed read)? Does each new test protect behaviour a user would
   see go wrong, named as such, rather than restate a value or an implementation detail? A
   test of a tuned value (a width, a gate, a tolerance) checks what the value is for, such as
   "the widest content fits at the minimum width", never the number itself, so the value can
   still be changed when that still holds.
4. **Simpler or documented way:** for platform, SDK or standard code, is there a documented
   or widely adopted approach the change ignores?
5. **Regressions:** can the change break a case that worked before: another mode, a host,
   real-time safety, performance on steady input?
6. **Requirements and decisions:** does the change contradict a functional requirement
   (FR-n), architecture decision (AD-n) or quality scenario (QS-n) in `docs/DESIGN.md`
   covering the behaviour it touches, without updating it in the same pull request? Search
   the document for the feature's terms and cite the ID.

## Severity

A finding is **Important** when it would let a defect or a regression reach a user, or breaks
a rule the product relies on:

- a correctness bug or regression, with a concrete scenario that triggers it;
- anything on the audio thread that can allocate, lock, block or do I/O;
- a fix without a test that fails without it, or a new failure branch with no test;
- a new test that only restates a value or an implementation detail (`expect(limit).toBe(920)`,
  a label's exact wording, the order of something no one reads in order): it fails on every
  intended change and on no defect, so it blocks the first and hides the absence of a test
  for the second;
- a break of "Rules the product relies on" in CLAUDE.md;
- a change that contradicts a requirement, decision or quality scenario in `docs/DESIGN.md`
  without updating it.

Everything else is a **nit**. Report at most three nits, and none once the pull request has
had a review: after the first round, report Important findings only, so that reviews
converge.

## Mutation testing

The mutation reports on a pull request (the `mutation`, `mutation-cpp` and `mutation-au` jobs)
list changes to the code that no test fails on. Each is a question, not a finding: a test is
worth adding only when a user would see that change go wrong. A change that alters nothing a user
relies on (mutants equivalent in practice, such as `<` for `<=` where the two values never meet,
wording, code that cannot run) is left as it is, or the code is removed when it does nothing.
Survivors are never reported as findings on their own, and no score is a goal.

The `mutation-triage` check applies this to every pull request whose mutation run leaves
mutants (`.github/workflows/mutation-triage.yml`): a `mutation-triager` gives each a verdict
(gap, equivalent, speed, tuned, dead, message, or tested by a test the mutation run does not run, such as an app test), a `review-verifier` checks each gap, and a
comment lists them all. It fails while a verified gap is left; the author adds a test that
fails with that mutant, or answers it, and a maintainer can waive it. A run that left mutants
not run (it reached its deadline, or a share of it failed) leaves a partial report: the mutants
it ran are judged all the same, but the check does not pass until the run is repeated or waived.

## Every finding

- cites the file and line;
- gives a concrete scenario: the input or state, and what goes wrong;
- is checked against the code before it is reported. Drop a finding you cannot back with the
  code; do not report style or ask for extra process.

The author fixes the findings they agree with and answers each one they reject, saying why.
