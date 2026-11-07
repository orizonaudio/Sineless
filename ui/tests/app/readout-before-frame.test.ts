// Before the engine's first frame there are no readouts: the tag says nothing is measured yet in
// Single ("—", a label that opens nothing) and "TT" in Multi, Compare on or off; the
// first frame names the channel, and in Single the tag becomes the button that chooses it.
import { describe, expect, test } from "vitest";

import {
  byId,
  choice,
  displayModeOf,
  displays,
  showDisplay,
  startBeforeFrames,
} from "./harness.ts";

describe("the readout tag before the first frame", () => {
  test.each(
    displays.map(
      (display) =>
        [
          display,
          display === "Single" ? "—" : "TT",
          display === "Single" ? "L" : "TT",
        ] as const,
    ),
  )("in %s, says %s, then %s", async (mode, before, after) => {
    const app = await startBeforeFrames();
    const tag = () => byId("readout-tag").textContent;
    await showDisplay(mode);
    await expect
      .element(choice("display-mode", displayModeOf(mode)))
      .toHaveAttribute("aria-pressed", "true");
    expect(window.orizonDebug?.frameCount ?? 0).toBe(0);
    await expect.poll(tag).toBe(before);
    await expect
      .element(byId("readout-details"))
      .toHaveAttribute("aria-disabled", "true");
    byId("readout-details").click();
    expect(byId("readout-popover").hidden).toBe(true);

    app.holdFrames(false);
    await expect.poll(tag).toBe(after);
    await expect
      .element(byId("readout-details"))
      .toHaveAttribute("aria-disabled", String(mode !== "Single"));
  });
});
