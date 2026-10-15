// Frozen, a frame shows audio held at the rate of its moment, which the host may have changed
// since: the page times what a frame shows (cursor readouts, offsets) at the rate the frame
// carries, not at the host's rate now.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { defaultSettingValues } from "../../../protocol/src/setting-values.ts";
import { protocolVersion } from "../../../protocol/src/version.ts";
import {
  mockBarQuarterNotes,
  mockTempo,
} from "../../src/bridge/mock-transport.ts";
import { parseQuantity } from "../../src/view/format.ts";
import { byId, choice, settle, startApp } from "./harness.ts";

// The frames' rate, and the host's after it changed.
const heldRate = 384000;
const hostRate = 44100;

describe("frames of audio held at another rate than the host's", () => {
  beforeAll(async () => {
    const app = await startApp({ sampleRate: heldRate });
    app.deliver({
      type: "snapshot",
      protocolVersion,
      settings: defaultSettingValues(),
      sampleRate: hostRate,
      inputChannels: 2,
    });
  });

  test("time the span between time cursors, and count its samples, at the frame's rate", async () => {
    await userEvent.click(choice("time-base", "Sec"));
    await userEvent.click(choice("display-mode", "Single"));
    if (byId("time-cursors").getAttribute("aria-pressed") !== "true") {
      await userEvent.click(byId("time-cursors"));
    }
    await settle();
    const text = () => byId("cursor-readout").textContent;
    await expect.poll(text).toMatch(/samples/u);
    const seconds = parseQuantity(
      /Δt ([^Δ]+?(?:ms|s))/u.exec(text())?.[1] ?? "",
      "time",
    );
    const samples = Number(/([\d.]+) samples/u.exec(text())?.[1]);
    // The cursors start a third of the span apart, which reads as that much time, held at the
    // frame's rate: its samples over its seconds.
    const spanSeconds = defaultSettingValues().timeSpan as number;
    expect(seconds).toBeCloseTo(spanSeconds / 3, 3);
    expect(samples / (seconds ?? NaN)).toBeCloseTo(heldRate, -3);
  });

  test("read Kick 2's offset in milliseconds at the frame's rate", async () => {
    await userEvent.click(choice("time-base", "Sec"));
    await userEvent.click(choice("display-mode", "Compare"));
    await settle();
    const check = (name: string) =>
      [...byId("lane-labels").querySelectorAll(".compare-check")].find(
        (button) => button.textContent === name,
      ) as HTMLElement;
    if (check("Kick 2").getAttribute("aria-checked") !== "true") {
      await userEvent.click(check("Kick 2"));
    }
    await expect
      .poll(() => check("Kick 2").getAttribute("aria-checked"))
      .toBe("true");
    await settle();
    // 499 samples at 384 kHz, as in high-sample-rate.test.ts.
    await expect
      .poll(
        () =>
          check("Kick 2").parentElement?.querySelector(".lane-readout")
            ?.textContent,
      )
      .toBe("+1.3 ms Ø · −0.93");
  });

  test("place the Beats grid's clock at the frame's rate", async () => {
    await userEvent.click(choice("time-base", "Beats"));
    await userEvent.click(choice("display-mode", "Single"));
    await settle();
    // Each label's position, "bar.beat", as quarter notes from the project start, and its
    // project time, "m:ss.sss".
    const labels = () =>
      [...document.querySelectorAll(".axis-detail")].map((detail) => {
        const [bar, beat] = (
          detail.parentElement?.firstChild?.textContent ?? ""
        )
          .split(".")
          .map(Number);
        const [minutes, seconds] = detail.textContent.split(":");
        return {
          quarterNotes:
            ((bar ?? NaN) - 1) * mockBarQuarterNotes + (beat ?? NaN) - 1,
          seconds: Number(minutes) * 60 + Number(seconds),
        };
      });
    // The transport has played at the mock tempo from the project start.
    const beat = 60 / mockTempo;
    const expectClocksOnBeats = async () => {
      await expect.poll(() => labels().length).toBeGreaterThan(1);
      for (const label of labels()) {
        expect(label.seconds).toBeCloseTo(label.quarterNotes * beat, 2);
      }
    };
    await expectClocksOnBeats();
    // Zoomed in, on a part of the span.
    const trace = byId("trace");
    const bounds = trace.getBoundingClientRect();
    trace.dispatchEvent(
      new WheelEvent("wheel", {
        deltaY: -400,
        ctrlKey: true,
        clientX: bounds.left + bounds.width / 2,
        clientY: bounds.top + bounds.height / 2,
        bubbles: true,
        cancelable: true,
      }),
    );
    await expect
      .poll(() => {
        const frame = window.orizonDebug?.lastFrame;
        return frame === undefined ? 1 : frame.viewEnd - frame.viewStart;
      })
      .toBeLessThan(0.9);
    // The labels are placed again from a zoomed frame when the sweep starts its next cycle.
    const cycle = window.orizonDebug?.lastFrame?.cycleStart;
    await expect
      .poll(() => window.orizonDebug?.lastFrame?.cycleStart, { timeout: 5000 })
      .not.toBe(cycle);
    await settle();
    await expectClocksOnBeats();
  });
});
