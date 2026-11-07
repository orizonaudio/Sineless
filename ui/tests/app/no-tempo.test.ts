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
      .map((display) => [display, display === "Single" ? "L" : "TT"] as const),
  )("in %s, the readout tag says %s", async (mode, tag) => {
    await showDisplay(mode);
    await expect.poll(chips).toEqual(["No tempo"]);
    await settle();
    expect(byId("readout-tag").textContent).toBe(tag);
  });

  // With no beats to draw, the page draws the equal divisions of Beats' own Time step, as it drew
  // ten before the grid could be chosen. This keeps that drawing; FR-9.1's "no grid" here is not
  // settled by it.
  test.each([
    ["coarse", 5],
    ["medium", 10],
    ["fine", 20],
  ] as const)(
    "draws Beats' %s Time step as %i equal divisions",
    async (time, divisions) => {
      app.transport.send({
        type: "setSetting",
        name: "gridTimeBeats",
        value: time,
      });
      await settle();
      const canvas = byId("grid") as HTMLCanvasElement;
      const y = Math.round(3 * window.devicePixelRatio);
      const row =
        canvas.getContext("2d")?.getImageData(0, y, canvas.width, 1).data ??
        new Uint8ClampedArray();
      let lines = 0;
      for (let x = 0; x < canvas.width; x++) {
        const drawn = (row[x * 4 + 3] ?? 0) > 127;
        const before = x > 0 && (row[(x - 1) * 4 + 3] ?? 0) > 127;
        lines += drawn && !before ? 1 : 0;
      }
      expect(lines).toBe(divisions - 1);
    },
  );

  test("places the bars once the host reports its tempo", async () => {
    app.transport.reportTempo();
    await expect.poll(chips).toContain("Sync");
    await expect.poll(() => timeAxisLabels().length).toBeGreaterThan(0);
    expect(chips()).not.toContain("No tempo");
  });
});
