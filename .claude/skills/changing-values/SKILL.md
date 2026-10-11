---
name: changing-values
description: Check what numeric values mean before merging, equalising, deriving or changing them. Use before merging constants into one, removing a difference between two copies of a value (e.g. engine vs UI), sizing a buffer or hold from another value, or changing a threshold, floor, gate, tolerance or margin - especially in protocol/src/limits.ts.
---

# Understand a value before changing it

Two numbers that look alike are not necessarily the same thing, and a difference that looks
like drift may be deliberate. Past mistakes here:

- One −100 dBFS floor was used for an RMS gate, a peak-to-peak gate, the level display and
  a relative gain, four different quantities; it was too high for some and too low for
  others.
- The engine's zoom limit (1/512) looked like drift from the UI's (1/256). It was a margin:
  making them equal let rounding reject about 1 in 170 views.
- Frozen lane holds were set to exactly the longest span, leaving no room for audio that
  arrives a block late, so lanes went blank.

## For each value involved, establish

1. **What it measures:** the quantity (peak, RMS, peak-to-peak, energy ratio), whether it is
   absolute (dBFS) or relative (dB), and its units.
2. **What it is for:** display, analysis gate, safety margin, tolerance or limit.
3. **Why it has its value:** its comment, the commit that introduced it
   (`git log -S'<value>'`), or the convention it follows.

## Then

- Merge only when all three match. Otherwise keep separate named constants side by side in
  `protocol/src/limits.ts`, each with its own comment.
- When two sides check the same limit, the side that receives a value tolerates rounding
  (e.g. `< limit * (1 - 1e-9)`).
- A value derived from another states its margin and why it is enough, next to the constant.
  Where a quantity must fit a capacity, `static_assert` it at the most demanding supported
  setting (e.g. 384 kHz).
- Check that any reason a comment gives is true against the numbers.
