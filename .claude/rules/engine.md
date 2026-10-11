---
paths:
  - "engine/**"
  - "plugin/**"
---

# Engine and plugin

- Look up map entries with `find` or `try_emplace`, not `operator[]`: it silently inserts a
  default entry, which once made a track skip its next match search.
- Mark a cache valid only when every input was read successfully; a failed read must be
  retried, not cached.
- The engine sends raw measurements, with NaN for none; rounding and display floors belong
  to the UI's formatters. Analysis gates (what audio is too quiet to analyse) stay in the
  engine, from `limits.ts`.
- Test the engine across sample rates up to 384 kHz and span lengths on both sides of every
  tolerance (`sweepLengthsSimilar` allows nothing below 1024 samples).
- After steady input the engine should do no repeated work; a change that rebuilds lanes or
  frames on every step is a bug even if the output looks right.
