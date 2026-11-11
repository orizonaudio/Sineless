// An instance the host has not played through has no tempo yet. In Beats, the page draws no bars
// or times from a made-up tempo: it says it has none, and places them once the host reports one.
import { describe, expect, test } from "vitest";

import {
  beforeApp,
  byId,
  channelRead,
  chips,
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

  // A frame with no tempo carries no lanes: in Single the Views field names the channel the frame
  // reads, Left, the first of the default views.
  test("in Single, reads the channel the frame names", async () => {
    await showDisplay("Single");
    await expect.poll(chips).toEqual(["No tempo"]);
    await settle();
    expect(channelRead()).toBe("Left");
  });

  // With no beats to draw, the page draws the equal divisions of Beats' own Density (FR-9.1).
  test.each([
    ["coarse", 5],
    ["medium", 10],
    ["fine", 20],
  ] as const)(
    "draws Beats' %s Density as %i equal divisions",
    async (time, divisions) => {
      app.transport.send({
        type: "setSetting",
        name: "gridDensity",
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
