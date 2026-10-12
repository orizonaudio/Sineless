## What and why

<!-- What was wrong or missing, for whom, and what this changes. Link the issue if there is one. -->

## Checklist

See [CONTRIBUTING.md](../CONTRIBUTING.md) for the definition of done.

- [ ] A bug fix starts with a test that fails without it; new behaviour has tests.
- [ ] The same cause was looked for, and fixed, in the other modes, time bases, platforms and readouts it reaches.
- [ ] `scripts/check.sh` (and `scripts\check.ps1` for Windows changes) pass locally, or the PR says which checks could not run.
- [ ] Nothing new runs on the audio thread that could allocate, lock or block.
- [ ] Numbers the engine and the UI share live in `protocol/src/limits.ts`.
- [ ] A fix for a platform, SDK, host or standard problem names its sources in a `Prior art:` note.
