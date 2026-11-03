import { describe, expect, test } from "vitest";

import { fullRateUpToHz, lineHysteresis } from "../../protocol/src/limits.ts";
import vectors from "../../protocol/tests/data/ticks-per-frame.json" with { type: "json" };
import { ticksPerFrame } from "../src/render/render-loop.ts";
import { randomFrom } from "./display-ticks.ts";

interface Case {
  readonly rateHz?: number;
  readonly line?: number;
  readonly factor?: number;
  readonly hysteresisShare?: number;
  readonly previous?: number;
  readonly n: number;
}

// A case's rate: a real display's, or a line (a whole multiple of the full rate) moved by a factor
// and a share of the hysteresis. The engine's FramePacing tests read the same cases.
function rateOf(entry: Case): number {
  return (
    entry.rateHz ??
    fullRateUpToHz *
      (entry.line ?? 1) *
      (entry.factor ?? 1) *
      (1 + (entry.hysteresisShare ?? 0) * lineHysteresis)
  );
}

describe("ticks per frame", () => {
  // Every tick up to 90 Hz and at most 90 frames a second on the common faster displays, each frame
  // held for the same whole number of ticks; a first measure falls on its side of a line, and a
  // later one changes n only once it is past the line by the hysteresis, so a rate measured near
  // a line does not flip n.
  test.each(vectors.cases as Case[])(
    "%o holds each frame for the right ticks",
    (entry) => {
      expect(ticksPerFrame(rateOf(entry), entry.previous)).toBe(entry.n);
    },
  );

  test("keeps n while a measure wanders inside the hysteresis around the line", () => {
    const random = randomFrom(11);
    let n = ticksPerFrame(fullRateUpToHz);
    const first = n;
    for (let index = 0; index < 10000; index++) {
      const share = 0.75 * lineHysteresis * (2 * random() - 1);
      n = ticksPerFrame(fullRateUpToHz * (1 + share), n);
      expect(n).toBe(first);
    }
  });
});
