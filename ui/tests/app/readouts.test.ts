// The top bar's readouts, through the real page: they measure the Mid in either time base, whatever
// the trigger source, and show levels with the shared formatters.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { TimeBase } from "../../../protocol/src/enums.ts";
import { formatDbfs, formatFrequency } from "../../src/view/format.ts";
import { mockFundamental } from "../../src/bridge/mock-transport.ts";
import { viewOrder } from "../../src/view/screen-state.ts";
import { byId, choice, settle, startApp } from "./harness.ts";

// The mock's channels (see mock-transport.ts): L is 0.6 sin; the Mid is half of L plus R.
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

// The Mid's RMS: of its two sines, 0.45 and 0.125.
const midRms = Math.sqrt((0.45 ** 2 + 0.125 ** 2) / 2);

// The time-base control's label for each time base of the protocol ("seconds" → "Sec").
const timeBaseLabels = Object.keys(TimeBase).map((name) =>
  name === "seconds" ? "Sec" : name.charAt(0).toUpperCase() + name.slice(1),
);

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

  // Every time base by its button's label, with every trigger source: the source picks what the
  // trigger reads, never what the readouts measure.
  test.each(
    timeBaseLabels.flatMap((timeBase) =>
      viewOrder.map(
        // The source control's label: the view's initial ("mid" → "M").
        (source) => [timeBase, source.charAt(0).toUpperCase()] as const,
      ),
    ),
  )(
    "measure the Mid in %s with the trigger on %s",
    async (timeBase, source) => {
      await userEvent.click(choice("time-base", "Sec"));
      await useSource(source);
      await userEvent.click(choice("time-base", timeBase));
      await settle();
      await expect.poll(() => byId("readout-tag").textContent).toBe("Mid");
      await expect
        .element(byId("peak"))
        .toHaveTextContent(formatDbfs(midPeak()));
      await expect.element(byId("rms")).toHaveTextContent(formatDbfs(midRms));
    },
  );

  test("show the frequency with the shared formatter", async () => {
    await userEvent.click(choice("time-base", "Sec"));
    await useSource("L");
    await settle();
    await expect
      .element(byId("frequency"))
      .toHaveTextContent(formatFrequency(mockFundamental));
  });

  test("use a true minus sign and no exponent notation", () => {
    const text = byId("peak").textContent;
    expect(text).toMatch(/^−\d+\.\d$/u);
  });
});
