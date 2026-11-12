// The Meter in the screen's corner, through the real page: off until its button is pressed, then
// Peak, RMS and Crest under the cursor readouts. It measures the channel chosen in the readout
// popover when it is drawn, the first view drawn when it is not, whatever the trigger source, and
// this track in Multi; it shows the frame's levels with the shared formatters, at once when
// switched on, also frozen; and the tallest corner panel fits the screen at the minimum size.
import { describe, expect, test } from "vitest";
import { page, userEvent } from "vitest/browser";

import { ChannelView, TimeBase } from "../../../protocol/src/enums.ts";
import {
  minimumEditorHeight,
  minimumEditorWidth,
} from "../../../protocol/src/limits.ts";
import { meterReadout } from "../../src/view/cursors.ts";
import { formatDbfs } from "../../src/view/format.ts";
import {
  mockFundamental,
  viewSample,
} from "../../src/bridge/mock-transport.ts";
import { viewOrder } from "../../src/view/screen-state.ts";
import {
  beforeApp,
  byId,
  channelRead,
  choice,
  cursorRow,
  displays,
  press,
  setPressed,
  settle,
  showDisplay,
  showViews,
  startApp,
  useReadout,
  type Started,
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

// What the Views field calls each channel read, as a screen reader says it.
const tagOf: Record<ChannelView, string> = {
  [ChannelView.left]: "Left",
  [ChannelView.right]: "Right",
  [ChannelView.mid]: "Mid",
  [ChannelView.side]: "Side",
};

// The titles of the corner panel's sections, in order, without the channel the Meter's names.
function sections(): string[] {
  return titles().map((title) => title.replace(/ \(.*\)$/u, ""));
}

function titles(): string[] {
  return [...byId("cursor-readout").querySelectorAll(".cursor-title")].map(
    (title) => title.textContent,
  );
}

// The Meter's title, which names the channel it reads (#393).
function meterTitle(): string | undefined {
  return titles().find((title) => title.startsWith("Meter"));
}

// The Meter's rows as shown, label and value.
function meterShown(): { label: string; value: string }[] {
  return ["Peak", "RMS", "Crest"].map((label) => ({
    label,
    value: cursorRow(label),
  }));
}

// The Meter's rows for the frame on screen, from its peak and RMS.
function meterOfFrame(): { label: string; value: string }[] {
  const frame = window.orizonDebug?.lastFrame;
  return meterReadout(frame?.peak ?? Number.NaN, frame?.rms ?? Number.NaN);
}

async function useSource(label: string): Promise<void> {
  await press(choice("source", label));
  await expect
    .element(choice("source", label))
    .toHaveAttribute("aria-pressed", "true");
}

async function expectMeterOf(view: ChannelView): Promise<void> {
  await expect.poll(channelRead).toBe(tagOf[view]);
  await expect.poll(meterTitle).toBe(`Meter (${tagOf[view]})`);
  const { peak, rms } = levelsOf(view);
  await expect.poll(() => cursorRow("Peak")).toBe(`${formatDbfs(peak)} dBFS`);
  await expect.poll(() => cursorRow("RMS")).toBe(`${formatDbfs(rms)} dBFS`);
}

describe("the Meter", () => {
  let app: Started;
  beforeApp(async () => {
    app = await startApp();
  });

  test("is off in a new instance, and its button asks the engine to show it", async () => {
    await expect
      .element(byId("meter-toggle"))
      .toHaveAttribute("aria-pressed", "false");
    expect(sections()).not.toContain("Meter");
    await setPressed(byId("meter-toggle"), true);
    expect(app.sent).toContainEqual(
      expect.objectContaining({
        type: "setSetting",
        name: "meter",
        value: "on",
      }),
    );
    await expect.poll(sections).toContain("Meter");
    await setPressed(byId("meter-toggle"), false);
    expect(sections()).not.toContain("Meter");
  });

  // Its button comes before the cursors' in the status bar, and its panel on top in the corner,
  // whichever was turned on first.
  test("comes before the time and level cursors, button and readouts", async () => {
    const buttons = [...document.querySelectorAll(".status-left .action")].map(
      (button) => button.id,
    );
    const meter = buttons.indexOf("meter-toggle");
    expect(buttons.slice(meter, meter + 3)).toEqual([
      "meter-toggle",
      "time-cursors",
      "level-cursors",
    ]);
    await setPressed(byId("time-cursors"), true);
    await setPressed(byId("level-cursors"), true);
    await setPressed(byId("meter-toggle"), true);
    await expect
      .poll(sections)
      .toEqual(["Meter", "Time cursors", "Level cursors"]);
    await setPressed(byId("time-cursors"), false);
    await setPressed(byId("level-cursors"), false);
    expect(sections()).toEqual(["Meter"]);
  });

  // Every time base by its button's label, with every channel chosen, every view drawn and the
  // trigger on another channel: the Meter measures the one chosen.
  test.each(
    timeBaseLabels.flatMap((timeBase) =>
      viewOrder.map((view) => [timeBase, view] as const),
    ),
  )("measures the channel chosen in %s: %s", async (timeBase, view) => {
    await setPressed(byId("meter-toggle"), true);
    await press(choice("time-base", "Trig"));
    await showViews(["L", "R", "M", "S"]);
    await useSource(view === "side" ? "L" : "S");
    await press(choice("time-base", timeBase));
    await useReadout(labelOf(view));
    await settle();
    await expectMeterOf(ChannelView[view]);
  });

  // Overlaid and stacked alike: the lanes change where traces are drawn, not what the Meter reads.
  test.each(["Overlay", "Stack"])(
    "measures the channel chosen with %s",
    async (layout) => {
      await setPressed(byId("meter-toggle"), true);
      await press(choice("time-base", "Trig"));
      await showViews(["L", "R", "M", "S"]);
      await press(choice("layout", layout));
      await useReadout("R");
      await settle();
      await expectMeterOf(ChannelView.right);
    },
  );

  // Hidden, the channel chosen gives way to the first view drawn, which the popover lights, the
  // hidden one disabled; shown again, it is measured again.
  test("measures the first view drawn while the one chosen is hidden", async () => {
    await setPressed(byId("meter-toggle"), true);
    await press(choice("time-base", "Trig"));
    await showViews(["L", "R", "M", "S"]);
    await useReadout("S");
    await showViews(["R", "M"]);
    await settle();
    await expectMeterOf(ChannelView.right);
    await userEvent.click(byId("views-details"));
    await expect
      .element(choice("readout-source", "R"))
      .toHaveAttribute("aria-pressed", "true");
    await expect.element(choice("readout-source", "S")).toBeDisabled();
    await expect.element(choice("readout-source", "L")).toBeDisabled();
    await userEvent.click(byId("views-details"));
    await showViews(["L", "R", "M", "S"]);
    await settle();
    await expectMeterOf(ChannelView.side);
  });

  // Single, Multi and Compare: the Meter shows what the frame measured (this track's Mid in Multi);
  // in Single, of the mock's sine on L, a sine's crest factor.
  test.each(displays)("shows the frame's levels in %s", async (display) => {
    await setPressed(byId("meter-toggle"), true);
    await press(choice("time-base", "Trig"));
    await showDisplay("Single");
    await showViews(["L", "R"]);
    await useReadout("L");
    await showDisplay(display);
    await settle();
    await expect.poll(meterShown).toEqual(meterOfFrame());
    if (display === "Single") {
      expect(cursorRow("Crest")).toBe("3.0 dB");
      expect(meterTitle()).toBe("Meter (Left)");
    } else {
      // The longest title keeps to the corner's width.
      expect(meterTitle()).toBe("Meter (This track)");
      const corner = Number.parseFloat(
        getComputedStyle(byId("screen")).getPropertyValue("--corner-width"),
      );
      expect(
        byId("cursor-readout").getBoundingClientRect().width,
      ).toBeLessThanOrEqual(corner);
    }
    await showDisplay("Single");
  });

  // Switched on, it shows the levels the page last took, not dashes until the next refresh: live,
  // and frozen, when no frame comes to refresh them.
  test.each([false, true])(
    "shows the levels at once when switched on, frozen %s",
    async (frozen) => {
      await press(choice("time-base", "Trig"));
      await settle();
      if (frozen) {
        await setPressed(byId("freeze"), true);
        await settle();
      }
      await setPressed(byId("meter-toggle"), false);
      await userEvent.click(byId("meter-toggle"));
      await expect
        .element(byId("meter-toggle"))
        .toHaveAttribute("aria-pressed", "true");
      expect(cursorRow("Peak")).not.toBe("—");
      expect(meterShown()).toEqual(meterOfFrame());
      expect(meterTitle()).toBe(`Meter (${channelRead() ?? ""})`);
      await setPressed(byId("freeze"), false);
    },
  );

  test("uses a true minus sign and no exponent notation", async () => {
    await setPressed(byId("meter-toggle"), true);
    await settle();
    expect(cursorRow("Peak")).toMatch(/^−\d+\.\d dBFS$/u);
  });
});

describe("the Meter on a mono track", () => {
  beforeApp(async () => {
    await startApp({ inputChannels: 1 });
  });

  test("measures its one channel, L", async () => {
    await setPressed(byId("meter-toggle"), true);
    await settle();
    await expectMeterOf(ChannelView.left);
  });
});

// The tallest corner panel: Time cursors in Beats (with the bars and beats between them), Level
// cursors and the Meter, at the editor's minimum size.
describe("the corner panel at the minimum size", () => {
  beforeApp(async () => {
    await page.viewport(minimumEditorWidth, minimumEditorHeight);
    await startApp();
  });

  test("fits the screen with everything on", async () => {
    await press(choice("time-base", "Beats"));
    await setPressed(byId("time-cursors"), true);
    await setPressed(byId("level-cursors"), true);
    await setPressed(byId("meter-toggle"), true);
    await settle();
    await expect
      .poll(sections)
      .toEqual(["Meter", "Time cursors", "Level cursors"]);
    const screen = byId("screen").getBoundingClientRect();
    const panel = byId("cursor-readout").getBoundingClientRect();
    expect(panel.bottom).toBeLessThanOrEqual(screen.bottom);
  });
});
