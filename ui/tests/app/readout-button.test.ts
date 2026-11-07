// The button beside the readouts names what they measure, short as the Views field's letters: TT
// for this track in Multi, L, R, M or S in Single, a dash before any frame; in full for screen
// readers. It keeps one width whichever it names, so nothing beside it moves.
import { describe, expect, test } from "vitest";

import {
  byId,
  displays,
  settle,
  showDisplay,
  showViews,
  startApp,
  startBeforeFrames,
  useReadout,
} from "./harness.ts";

const tag = () => byId("readout-tag").textContent;
const label = () => byId("readout-details").getAttribute("aria-label");
const width = () => byId("readout-details").getBoundingClientRect().width;

const named = { L: "Left", R: "Right", M: "Mid", S: "Side" } as const;

describe("the readouts' button", () => {
  test("names each channel chosen in Single, then TT in Multi, at one width", async () => {
    await startApp();
    await settle();
    // Every view drawn, so each can be chosen.
    await showViews(["L", "R", "M", "S"]);
    const widths = new Set<number>();
    for (const [letter, name] of Object.entries(named)) {
      await useReadout(letter);
      await expect.poll(tag).toBe(letter);
      expect(label()).toBe(`Readouts: ${name}`);
      widths.add(Math.round(width() * 100));
    }
    for (const display of displays.filter((shown) => shown !== "Single")) {
      await showDisplay(display);
      await expect.poll(tag).toBe("TT");
      expect(label()).toBe("Readouts: This track");
      widths.add(Math.round(width() * 100));
    }
    expect(widths.size).toBe(1);
  });

  test("names a mono track's one channel L", async () => {
    await startApp({ inputChannels: 1 });
    await settle();
    await expect.poll(tag).toBe("L");
    expect(label()).toBe("Readouts: Left");
  });

  test("shows a dash before the first frame, as wide as when it names a channel", async () => {
    const app = await startBeforeFrames();
    expect(tag()).toBe("—");
    const before = width();
    app.holdFrames(false);
    await expect.poll(tag).toBe("L");
    expect(width()).toBeCloseTo(before, 1);
  });
});
