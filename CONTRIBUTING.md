# Contributing

Design: [docs/DESIGN.md](docs/DESIGN.md). Task workflow: [docs/tasks/README.md](docs/tasks/README.md).

## Definition of Done

Every task, in addition to its own acceptance criteria:

- [ ] All checks in DESIGN.md section 8.2 pass. Until a remote CI exists, they are run locally.
- [ ] New or changed behaviour is covered by tests.
- [ ] Code follows the naming conventions (DESIGN.md 8.3) and is formatted and linted.
- [ ] Nothing new runs on the audio thread that could allocate, lock or block (QG-1).
- [ ] Any new dependency was added to DESIGN.md 8.1 after a decision.
- [ ] DESIGN.md is updated if a requirement, decision or documented behaviour changed.
- [ ] The work is committed with the task ID in the message, and BOARD.md is updated.

## Releasing

1. Set the version in `project(... VERSION x.y.z ...)` in `CMakeLists.txt`.
2. Run `scripts/check.sh`. Every check must pass.
3. Run `scripts/package.sh`. It builds the Release bundle, verifies both architectures and the macOS 13 minimum, and writes `build/release/O-Scope-<version>-macOS.zip`.
4. Install the zipped bundle and do the manual test pass in Ableton Live (DESIGN 8.2).
5. Tag the commit `vx.y.z` and attach the zip to a GitHub release.
