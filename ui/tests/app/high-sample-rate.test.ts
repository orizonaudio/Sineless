// The page at the highest sample rate a host runs, 384 kHz: offsets and cursor readouts follow
// the rate the engine reports, not 48 kHz.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { parseQuantity } from "../../src/view/format.ts";
import { byId, choice, settle, startApp } from "./harness.ts";

describe("at 384 kHz", () => {
  beforeAll(async () => {
    await startApp({ sampleRate: 384000 });
  });

  test("reads Kick 2's offset in milliseconds, as at any rate", async () => {
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
    // 499 samples at 384 kHz.
    await expect
      .poll(
        () =>
          check("Kick 2").parentElement?.querySelector(".lane-readout")
            ?.textContent,
      )
      .toBe("+1.3 ms Ø · −0.93");
  });

  test("counts the samples between time cursors at the reported rate", async () => {
    await userEvent.click(choice("time-base", "Sec"));
    await userEvent.click(choice("display-mode", "Single"));
    if (byId("time-cursors").getAttribute("aria-pressed") !== "true") {
      await userEvent.click(byId("time-cursors"));
    }
    await settle();
    const text = () => byId("cursor-readout").textContent;
    await expect.poll(text).toMatch(/samples/u);
    // "Δt 500 ms" and "192000.0 samples": samples over seconds is the rate, to Δt's three
    // significant digits.
    const seconds = parseQuantity(
      /Δt ([^Δ]+?(?:ms|s))/u.exec(text())?.[1] ?? "",
      "time",
    );
    const samples = Number(/([\d.]+) samples/u.exec(text())?.[1]);
    expect(seconds).toBeGreaterThan(0);
    expect(samples / (seconds ?? NaN)).toBeCloseTo(384000, -3);
  });
});
