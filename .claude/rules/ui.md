---
paths:
  - "ui/**"
  - "protocol/**"
---

# UI and protocol

- Readouts round first, then choose the unit and precision: never exponent notation, never
  "−0.0", and U+2212 for minus. Each typed field parses what its formatter prints, in its
  own units only.
- One function decides each shared rule (which channel readouts, snapping and hover measure;
  what counts as cancelling); readouts, cursors, hover and shading call it rather than
  re-deriving it.
- Nothing in the window is cropped, hidden or moved as it is resized. The minimum size
  (`minimumEditorWidth` in `protocol/src/limits.ts`, the page's min-width) fits the widest
  content; content of unbounded width goes where there is room for it.
- Help text reads its numbers from `protocol/src/limits.ts` and describes what the code does
  in every mode it applies to.
- Changing the frame layout means bumping `protocolVersion` (`protocol/src/version.ts`) and
  updating both shared golden frames in `protocol/tests/data/`, which the C++ encoder tests and
  the TypeScript decoder tests read.
