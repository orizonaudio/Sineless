// Before the engine's first frame there are no readouts: the tag says nothing is measured yet in
// Single ("—", a label that opens nothing) and "This track" in Multi and Compare; the first frame
// names the channel, and in Single the tag becomes the button that chooses it.
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { DisplayMode } from "../../../protocol/src/enums.ts";
import { byId, choice, startBeforeFrames } from "./harness.ts";

describe("the readout tag before the first frame", () => {
  test.each(
    Object.keys(DisplayMode).map((key) => [
      key.charAt(0).toUpperCase() + key.slice(1),
      key === "single" ? "—" : "This track",
      key === "single" ? "Left" : "This track",
    ]),
  )("in %s, says %s, then %s", async (mode, before, after) => {
    const app = await startBeforeFrames();
    const tag = () => byId("readout-tag").textContent;
    await userEvent.click(choice("display-mode", mode));
    await expect
      .element(choice("display-mode", mode))
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
