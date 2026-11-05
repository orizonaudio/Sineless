// The status bar's readouts, through the real page: they measure the channel chosen in their
// popover when it is drawn, the first view drawn when it is not, whatever the trigger source, and
// show levels with the shared formatters.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { ChannelView, TimeBase } from "../../../protocol/src/enums.ts";
import { formatDbfs, formatFrequency } from "../../src/view/format.ts";
import {
  mockFundamental,
  viewSample,
} from "../../src/bridge/mock-transport.ts";
import { viewNames, viewOrder } from "../../src/view/screen-state.ts";
import {
  byId,
  choice,
  press,
  settle,
  showViews,
  startApp,
  useReadout,
} from "./harness.ts";

// A view's peak and RMS over one cycle of the mock's channels (see mock-transport.ts).
function levelsOf(view: ChannelView): { peak: number; rms: number } {
  const steps = 100000;
  let peak = 0;
  let sum = 0;
  for (let step = 0; step < steps; step++) {
    const value = viewSample(view, step / steps / mockFundamental);
    peak = Math.max(peak, Math.abs(value));
    sum += value * value;
  }
  return { peak, rms: Math.sqrt(sum / steps) };
}

// The time-base control's label for each time base of the protocol ("trigger" → "Trig").
const timeBaseLabels = Object.keys(TimeBase).map((name) =>
  name === "trigger" ? "Trig" : name.charAt(0).toUpperCase() + name.slice(1),
);

// A view's label in the controls: its initial ("mid" → "M").
const labelOf = (name: string) => name.charAt(0).toUpperCase();

async function useSource(label: string): Promise<void> {
  await press(choice("source", label));
  await expect
    .element(choice("source", label))
    .toHaveAttribute("aria-pressed", "true");
}

async function expectReadoutsOf(view: ChannelView): Promise<void> {
  await expect
    .poll(() => byId("readout-tag").textContent)
    .toBe(viewNames[view]);
  const { peak, rms } = levelsOf(view);
  await expect.element(byId("peak")).toHaveTextContent(formatDbfs(peak));
  await expect.element(byId("rms")).toHaveTextContent(formatDbfs(rms));
}

describe("readouts", () => {
  beforeAll(async () => {
    await startApp();
  });

  // Every time base by its button's label, with every channel chosen, every view drawn and the
  // trigger on another channel: the readouts measure the one chosen.
  test.each(
    timeBaseLabels.flatMap((timeBase) =>
      viewOrder.map((view) => [timeBase, view] as const),
    ),
  )("measure the channel chosen in %s: %s", async (timeBase, view) => {
    await userEvent.click(choice("time-base", "Trig"));
    await useSource(view === "side" ? "L" : "S");
    await userEvent.click(choice("time-base", timeBase));
    await showViews(["L", "R", "M", "S"]);
    await useReadout(labelOf(view));
    await settle();
    await expectReadoutsOf(ChannelView[view]);
  });

  // Hidden, the channel chosen gives way to the first view drawn, which the popover lights, the
  // hidden one disabled; shown again, it is measured again.
  test("measure the first view drawn while the one chosen is hidden", async () => {
    await userEvent.click(choice("time-base", "Trig"));
    await showViews(["L", "R", "M", "S"]);
    await useReadout("S");
    await showViews(["R", "M"]);
    await settle();
    await expectReadoutsOf(ChannelView.right);
    await userEvent.click(byId("readout-details"));
    await expect
      .element(choice("readout-source", "R"))
      .toHaveAttribute("aria-pressed", "true");
    await expect.element(choice("readout-source", "S")).toBeDisabled();
    await expect.element(choice("readout-source", "L")).toBeDisabled();
    await userEvent.click(byId("readout-details"));
    await showViews(["L", "R", "M", "S"]);
    await settle();
    await expectReadoutsOf(ChannelView.side);
    await userEvent.click(byId("readout-details"));
    await expect
      .element(choice("readout-source", "S"))
      .toHaveAttribute("aria-pressed", "true");
    await userEvent.click(byId("readout-details"));
  });

  test("show the frequency with the shared formatter", async () => {
    await userEvent.click(choice("time-base", "Trig"));
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
