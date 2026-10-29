// A frozen Compare frame held at 384 kHz, after the host changed to 44.1 kHz: on the real
// engine's frames, the page times what the frame shows at the rate it was held at.
import { beforeAll, describe, expect, test, vi } from "vitest";
import { userEvent } from "vitest/browser";

import { formatOffset, parseQuantity } from "../../src/view/format.ts";
import { laneReadout } from "./compare-readout.ts";
import type { ReplayTransport } from "./replay-transport.ts";
import { byId, settle, startRecorded, type Started } from "./harness.ts";

const heldRate = 384000;
const lateSamples = 499; // The copy's delay in the recording.

describe("a recorded frozen Compare frame after the host changed rate", () => {
  let app: Started<ReplayTransport>;
  beforeAll(async () => {
    app = await startRecorded("FrozenHeldRate_384kThen44k1");
    await settle();
    await userEvent.click(byId("freeze"));
    await vi.waitFor(() => {
      expect(window.orizonDebug?.lastFrame?.frozen).toBe(true);
    });
    app.transport.hostEvent();
    await settle();
  });

  test("time the span between time cursors, and count its samples, at the held rate", async () => {
    await userEvent.click(byId("time-cursors"));
    const text = () => byId("cursor-readout").textContent;
    await expect.poll(text).toMatch(/samples/u);
    const seconds = parseQuantity(
      /Δt ([^Δ]+?(?:ms|s))/u.exec(text())?.[1] ?? "",
      "time",
    );
    const samples = Number(/([\d.]+) samples/u.exec(text())?.[1]);
    expect(samples / (seconds ?? Number.NaN)).toBeCloseTo(heldRate, -3);
  });

  test("read the late copy's offset in milliseconds at the held rate", async () => {
    await expect
      .poll(() => laneReadout("Late copy"))
      .toContain(formatOffset((lateSamples / heldRate) * 1000));
  });
});
