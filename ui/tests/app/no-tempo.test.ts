// An instance the host has not played through has no tempo yet. In Beats, the page draws no bars
// or times from a made-up tempo: it says it has none, and places them once the host reports one.
import { describe, expect, test } from "vitest";

import {
  beforeApp,
  byId,
  chips,
  displays,
  settle,
  showDisplay,
  startApp,
  type Started,
} from "./harness.ts";

// The time axis's labels (bar positions in Beats, times in Seconds): those along the screen, not
// the level labels down its left edge.
function timeAxisLabels(): string[] {
  return [...byId("labels").querySelectorAll<HTMLElement>(".axis-label")]
    .filter((label) => label.style.left !== "13px")
    .map((label) => label.firstChild?.textContent ?? "");
}

describe("before the host reports a tempo", () => {
  let app: Started;
  beforeApp(async () => {
    app = await startApp({ hostTempo: false });
  });

  test("says so, and places no bars", async () => {
    await expect.poll(chips).toEqual(["No tempo"]);
    await settle();
    expect(timeAxisLabels()).toEqual([]);
  });

  // A frame with no tempo carries no lanes, even in Multi, Compare off or on: the readout tag
  // still names what those modes measure, this track, by the frame's lane set, and in Single the channel
  // the frame names (Left, the first of the default views). Single comes last, so the tests after
  // these start where the page starts.
  test.each(
    [...displays]
      .sort((a, b) => Number(a === "Single") - Number(b === "Single"))
      .map(
        (display) =>
          [display, display === "Single" ? "Left" : "This track"] as const,
      ),
  )("in %s, the readout tag says %s", async (mode, tag) => {
    await showDisplay(mode);
    await expect.poll(chips).toEqual(["No tempo"]);
    await settle();
    expect(byId("readout-tag").textContent).toBe(tag);
  });

  test("places the bars once the host reports its tempo", async () => {
    app.transport.reportTempo();
    await expect.poll(chips).toContain("Sync");
    await expect.poll(() => timeAxisLabels().length).toBeGreaterThan(0);
    expect(chips()).not.toContain("No tempo");
  });
});
