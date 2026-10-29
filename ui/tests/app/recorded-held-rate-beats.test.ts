// A frozen Beats sweep held at 384 kHz, after the host changed to 44.1 kHz: on the real engine's
// frames, the grid's clock keeps the tempo the audio was played at.
import { beforeAll, describe, expect, test, vi } from "vitest";
import { userEvent } from "vitest/browser";

import type { ReplayTransport } from "./replay-transport.ts";
import { byId, settle, startRecorded, type Started } from "./harness.ts";

// The recording's host: 120 BPM in 4/4, from the project start.
const secondsPerBeat = 60 / 120;
const beatsPerBar = 4;

// Each label's position, "bar.beat", in beats from the project start, and its project time,
// "m:ss.sss".
function labels(): { beats: number; seconds: number }[] {
  return [...document.querySelectorAll(".axis-detail")].map((detail) => {
    const [bar, beat] = (detail.parentElement?.firstChild?.textContent ?? "")
      .split(".")
      .map(Number);
    const [minutes, seconds] = detail.textContent.split(":");
    return {
      beats: ((bar ?? NaN) - 1) * beatsPerBar + (beat ?? NaN) - 1,
      seconds: Number(minutes) * 60 + Number(seconds),
    };
  });
}

describe("a recorded frozen Beats sweep after the host changed rate", () => {
  let app: Started<ReplayTransport>;
  beforeAll(async () => {
    app = await startRecorded("FrozenHeldRateBeats_384kThen44k1");
    await settle();
    await userEvent.click(byId("freeze"));
    await vi.waitFor(() => {
      expect(window.orizonDebug?.lastFrame?.frozen).toBe(true);
    });
    app.transport.hostEvent();
    await settle();
  });

  test("place the grid's clock on the beats at the tempo played", async () => {
    await expect.poll(() => labels().length).toBeGreaterThan(1);
    for (const label of labels()) {
      expect(label.seconds).toBeCloseTo(label.beats * secondsPerBeat, 2);
    }
  });
});
