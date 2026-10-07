# Contributing

## Definition of Done

Every change:

- [ ] `scripts/check.sh` passes locally. CI runs the same script, on demand.
- [ ] New or changed behaviour is covered by tests.
- [ ] Code follows the naming conventions and is formatted and linted.
- [ ] Nothing new runs on the audio thread that could allocate, lock or block.
- [ ] Any new dependency was agreed on first and is pinned.

## Releasing

1. Set the version in `project(... VERSION x.y.z ...)` in `CMakeLists.txt`.
2. Run `scripts/check.sh`. Every check must pass.
3. Run `scripts/package.sh`. It builds the Release bundle, verifies both architectures and the macOS 13 minimum, and writes `build/release/O-Scope-<version>-macOS.zip`.
4. Install the zipped bundle and do a manual test pass in Ableton Live.
5. Tag the commit `vx.y.z` and attach the zip to a GitHub release.
