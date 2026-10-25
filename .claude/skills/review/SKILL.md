---
name: review
description: Review a change before a pull request is opened - specialist reviewers in parallel, then one verifier per Important finding. Use before opening a pull request, giving it the base and head commits (by default the merge base with origin/main and HEAD).
---

# Review before a push

The `ai-review` check on the pull request runs the same reviewers and verification, so a
change that passes here should pass there. REVIEW.md says what counts as a finding.

## 1. The change

Take the base and head you were given, or `git merge-base origin/main HEAD` and `HEAD`. List
the changed files with `git diff --name-only <base> <head>`.

## 2. Specialists, in parallel

Launch these subagents in one message, giving each the range (`git diff <base> <head>`) and
nothing else about the change:

- `review-correctness`, `review-rules` and `review-tests`, always;
- `review-realtime`, when C++ or Objective-C++ (`.cpp`, `.cc`, `.h`, `.hpp`, `.mm`) under
  `engine/`, `plugin/` or `editor/` changed.

When only documentation changed (Markdown, or files under `docs/`, as in
`scripts/changed-code.ts`), run `review-rules` alone.

## 3. Verify each Important finding

Launch one `review-verifier` per Important finding, in one message, giving it the finding
(file, line, title, scenario, evidence) and the range. Drop a finding its verifier rejects
or downgrades to a nit; keep one whose verifier returns no verdict, as the check does. Nits
are not verified; keep at most three.

## 4. Act on what is left

Fix each verified Important finding (one issue per commit, with its test), or answer it in
the pull request saying why it does not hold. Show the findings the verifiers rejected or
downgraded too, in one line each, so a wrong rejection can be noticed.
