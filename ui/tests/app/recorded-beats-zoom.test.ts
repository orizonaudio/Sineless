// A live Beats sweep of one bar at 120 BPM, on the real engine's frames, then zoomed to the
// narrowest view: the grid's clock keeps the tempo, whatever part of the bar is shown.
import { beforeAll, describe, expect, test } from "vitest";

import { byId, settle, startRecorded } from "./harness.ts";
import { zoomToNarrowest } from "./zoom-in.ts";

// The recording's host: 120 BPM in 4/4, from the project start.
const secondsPerBeat = 60 / 120;
const barBeats = 4;

// Each grid label's place on the screen (0 to 1) and its project time, "m:ss.sss", in seconds.
function clocks(): { x: number; seconds: number }[] {
  const width = byId("screen").clientWidth;
  return [...document.querySelectorAll(".axis-detail")].map((detail) => {
    const label = detail.parentElement as HTMLElement;
    const left = label.style.left;
    const x = left.endsWith("%")
      ? Number.parseFloat(left) / 100
      : Number.parseFloat(left) / width;
    const [minutes, seconds] = detail.textContent.split(":");
    return { x, seconds: Number(minutes) * 60 + Number(seconds) };
  });
}

// Whether the clocks run at the tempo across `screenSeconds` of project time on the screen,
// within the clock's millisecond. Labels sit a fixed distance from their lines, which comparing
// them cancels; the first may be held in from the screen's edge, so it is left out.
function atTheTempo(screenSeconds: number): boolean {
  const shown = clocks().slice(1);
  const first = shown[0];
  return (
    shown.length > 1 &&
    first !== undefined &&
    shown.every(
      ({ x, seconds }) =>
        Math.abs(seconds - first.seconds - (x - first.x) * screenSeconds) <=
        0.0006,
    )
  );
}

describe("a recorded live Beats sweep of one bar", () => {
  beforeAll(async () => {
    await startRecorded("BeatsLive_48k");
  });

  test("clocks the grid at the tempo", async () => {
    await settle();
    await expect.poll(() => atTheTempo(barBeats * secondsPerBeat)).toBe(true);
  });

  // The labels are placed again from the zoomed frame when the window is resized, so its view's
  // width, not the whole span's, times them.
  test("clocks the grid at the tempo when zoomed to the narrowest view, and resized", async () => {
    await zoomToNarrowest();
    window.dispatchEvent(new Event("resize"));
    await settle();
    const frame = window.orizonDebug?.lastFrame;
    const shown = (frame?.viewEnd ?? 1) - (frame?.viewStart ?? 0);
    await expect
      .poll(() => atTheTempo(barBeats * secondsPerBeat * shown), {
        message: JSON.stringify(clocks()),
      })
      .toBe(true);
  });
});
