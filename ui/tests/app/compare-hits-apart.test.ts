// Compare, a track whose short hits arrive late enough that the two never sound together as
// they are: the engine sends no correlation for it and still an offset, and the label shows the
// offset beside "—", not "—" alone.
import { describe, expect, test } from "vitest";

import { laneReadout } from "./compare-readout.ts";
import {
  beforeApp,
  choice,
  press,
  setCompare,
  settle,
  startApp,
} from "./harness.ts";

beforeApp(async () => {
  // Five tracks: the fifth is the far hi-hat microphone, 7 ms late.
  await startApp({ trackCount: 5 });
});

describe("a compared track that matches only at an offset", () => {
  test("shows its offset, and no correlation", async () => {
    await press(choice("time-base", "Trig"));
    await press(choice("display-mode", "Multi"));
    await setCompare(true);
    await settle();
    await expect.poll(() => laneReadout("Hat far")).toBe("+7.0 ms · —");
  });

  // Beside the difference a track's readout says what Align takes out: for a track that is only
  // late, its delay and nothing more.
  test("shows its delay alone beside the difference", async () => {
    await setCompare("Diff");
    await expect.poll(() => laneReadout("Hat far")).toBe("+7.0 ms");
    await setCompare("Sum");
  });
});
