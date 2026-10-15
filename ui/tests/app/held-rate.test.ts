// Frozen, a frame shows audio held at the rate of its moment, which the host may have changed
// since: the page times what a frame shows (cursor readouts, offsets) at the rate the frame
// carries, not at the host's rate now.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { defaultSettingValues } from "../../../protocol/src/setting-values.ts";
import { protocolVersion } from "../../../protocol/src/version.ts";
import { mockTempo } from "../../src/bridge/mock-transport.ts";
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

  test("count the samples between time cursors at the frame's rate", async () => {
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
    expect(seconds).toBeGreaterThan(0);
    expect(samples / (seconds ?? NaN)).toBeCloseTo(heldRate, -3);
  });

  test("place the Beats grid's clock at the frame's rate", async () => {
    await userEvent.click(choice("time-base", "Beats"));
    await userEvent.click(choice("display-mode", "Single"));
    await settle();
    // Each label's project time, "m:ss.sss", left to right.
    const clocks = () =>
      [...document.querySelectorAll(".axis-detail")].map((node) => {
        const [minutes, seconds] = node.textContent.split(":");
        return Number(minutes) * 60 + Number(seconds);
      });
    await expect.poll(() => clocks().length).toBeGreaterThan(1);
    // Labels fall on beats, so the clock steps by whole beats.
    const beat = 60 / mockTempo;
    const times = clocks();
    for (let index = 1; index < times.length; index++) {
      const beats = ((times[index] ?? NaN) - (times[index - 1] ?? NaN)) / beat;
      expect(beats).toBeGreaterThan(0);
      expect(Math.abs(beats - Math.round(beats))).toBeLessThan(0.01);
    }
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
});
