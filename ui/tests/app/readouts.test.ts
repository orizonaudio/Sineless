// The top bar's readouts, through the real page: they measure the trigger source in Seconds mode
// and the Mid in Beats mode, where the trigger does not apply, and show levels with the shared
// formatters.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { formatDbfs, formatFrequency } from "../../src/view/format.ts";
import { mockFundamental } from "../../src/bridge/mock-transport.ts";
import { byId, choice, settle, startApp } from "./harness.ts";

// The mock's channels (see mock-transport.ts): L is 0.6 sin; the Mid is half of L plus R.
const leftPeak = 0.6;
function midPeak(): number {
  let peak = 0;
  for (let step = 0; step < 100000; step++) {
    const phase = (2 * Math.PI * step) / 100000;
    peak = Math.max(
      peak,
      Math.abs(0.45 * Math.sin(phase) + 0.125 * Math.sin(2 * phase + 0.7)),
    );
  }
  return peak;
}

async function useSource(label: string): Promise<void> {
  if (byId("trigger-popover").hidden) {
    await userEvent.click(byId("trigger-details"));
  }
  await userEvent.click(choice("source", label));
  await expect
    .element(choice("source", label))
    .toHaveAttribute("aria-pressed", "true");
}

describe("readouts", () => {
  beforeAll(async () => {
    await startApp();
  });

  test("measure the trigger source in Seconds mode", async () => {
    await userEvent.click(choice("time-base", "Sec"));
    await useSource("L");
    await settle();
    await expect.element(byId("peak")).toHaveTextContent(formatDbfs(leftPeak));
    await expect
      .element(byId("rms"))
      .toHaveTextContent(formatDbfs(leftPeak / Math.SQRT2));
    await expect
      .element(byId("frequency"))
      .toHaveTextContent(formatFrequency(mockFundamental));

    await useSource("M");
    await settle();
    await expect.element(byId("peak")).toHaveTextContent(formatDbfs(midPeak()));
  });

  test("measure the Mid in Beats mode, whatever the trigger source", async () => {
    await useSource("L");
    await userEvent.click(choice("time-base", "Beats"));
    await settle();
    await expect.element(byId("peak")).toHaveTextContent(formatDbfs(midPeak()));
  });

  test("use a true minus sign and no exponent notation", () => {
    const text = byId("peak").textContent;
    expect(text).toMatch(/^−\d+\.\d$/u);
  });
});
