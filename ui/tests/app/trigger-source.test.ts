// The trigger reads only views drawn, as the readouts measure them (DESIGN.md 1.2.1): the Source
// row lights the view the trigger reads and disables the views not drawn, the choice is kept while
// its view is hidden, and the level marker sits in that view's lane in its colour. On a mono track
// only L. Multi and Compare trigger on this track's Mid and hide the row.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { DisplayMode } from "../../../protocol/src/enums.ts";
import {
  byId,
  choice,
  press,
  setPressed,
  settle,
  showViews,
  startApp,
  startBeforeFrames,
  type Started,
} from "./harness.ts";

const colourOf: Record<string, string> = {
  L: "var(--sl-left)",
  R: "var(--sl-right)",
  M: "var(--sl-mid)",
  S: "var(--sl-side)",
};

const marker = () => byId("trigger-marker");
const markerColour = () => marker().style.getPropertyValue("--marker");
const markerTop = () => Number.parseFloat(marker().style.top);

async function useSource(label: string): Promise<void> {
  await press(choice("source", label));
  await settle();
  await userEvent.click(byId("trigger-details"));
}

// The Source row's lit label, and the labels it lets be chosen.
async function sourceRow(): Promise<{ lit: string; enabled: string[] }> {
  if (byId("trigger-popover").hidden) {
    await userEvent.click(byId("trigger-details"));
  }
  const buttons = [...byId("source").querySelectorAll("button")];
  const row = {
    lit:
      buttons.find((button) => button.getAttribute("aria-pressed") === "true")
        ?.textContent ?? "",
    enabled: buttons
      .filter((button) => !button.disabled)
      .map((button) => button.textContent),
  };
  await userEvent.click(byId("trigger-details"));
  return row;
}

describe("the trigger source on a stereo track", () => {
  let app: Started;
  beforeAll(async () => {
    app = await startApp();
    await userEvent.click(choice("display-mode", "Single"));
    await userEvent.click(choice("time-base", "Trig"));
    await userEvent.click(choice("layout", "Stack"));
  });

  test.each([false, true])(
    "follows the views drawn (frozen: %s)",
    async (frozen) => {
      await setPressed(byId("freeze"), false);
      await showViews(["L", "R"]);
      await useSource("L");
      await settle();
      const leftTop = markerTop();
      await press(choice("source", "R"));
      await settle();
      await userEvent.click(byId("trigger-details"));
      const rightTop = markerTop();
      expect(rightTop, "R's lane is below L's").toBeGreaterThan(leftTop);
      await setPressed(byId("freeze"), frozen);

      // The default source, the Mid, is not drawn: the trigger reads L.
      await press(choice("source", "R"));
      await userEvent.click(byId("trigger-details"));
      app.transport.send({
        type: "setSetting",
        name: "triggerSource",
        value: "mid",
      });
      await settle();
      expect(await sourceRow()).toEqual({ lit: "L", enabled: ["L", "R"] });
      expect(markerColour()).toBe(colourOf["L"]);
      expect(markerTop()).toBe(leftTop);

      await useSource("R");
      expect(await sourceRow()).toEqual({ lit: "R", enabled: ["L", "R"] });
      expect(markerColour()).toBe(colourOf["R"]);
      expect(markerTop()).toBe(rightTop);

      // R hidden: L, and R kept as the choice; shown again, R without a click.
      await showViews(["L", "M"]);
      await settle();
      expect(await sourceRow()).toEqual({ lit: "L", enabled: ["L", "M"] });
      expect(markerColour()).toBe(colourOf["L"]);
      await showViews(["L", "R"]);
      await settle();
      expect(await sourceRow()).toEqual({ lit: "R", enabled: ["L", "R"] });
      expect(markerColour()).toBe(colourOf["R"]);
      await setPressed(byId("freeze"), false);
    },
  );

  // Multi and Compare trigger on this track's Mid: the row is hidden, and the marker takes the own
  // lane's colour, whatever the source.
  test.each(
    Object.keys(DisplayMode)
      .filter((key) => key !== "single")
      .map((key) => key.charAt(0).toUpperCase() + key.slice(1)),
  )("is not used in %s", async (mode) => {
    await showViews(["L", "R"]);
    await useSource("R");
    await userEvent.click(choice("display-mode", mode));
    await settle();
    expect(byId("source-row").hidden).toBe(true);
    expect(markerColour()).not.toBe(colourOf["R"]);
    expect(markerColour()).toMatch(/^(rgb|#)/u);
    // Back to Single, the row lights the source chosen again, from the Single frame.
    await userEvent.click(choice("display-mode", "Single"));
    await settle();
    expect((await sourceRow()).lit).toBe("R");
  });
});

describe("the trigger source on a mono track", () => {
  let app: Started;
  beforeAll(async () => {
    app = await startApp({ inputChannels: 1 });
    await userEvent.click(choice("display-mode", "Single"));
    await userEvent.click(choice("time-base", "Trig"));
  });

  // #247: the source set to S, the trigger reads L, the one channel; the row and the marker say so.
  test("offers and lights L alone, the marker in L's colour", async () => {
    app.transport.send({
      type: "setSetting",
      name: "triggerSource",
      value: "side",
    });
    await settle();
    expect(await sourceRow()).toEqual({ lit: "L", enabled: ["L"] });
    expect(markerColour()).toBe(colourOf["L"]);
  });

  test("follows the track turning stereo and back", async () => {
    app.transport.changeChannels(2, true);
    await showViews(["L", "S"]);
    await settle();
    expect(await sourceRow()).toEqual({ lit: "S", enabled: ["L", "S"] });
    expect(markerColour()).toBe(colourOf["S"]);
    app.transport.changeChannels(1, true);
    await settle();
    expect(await sourceRow()).toEqual({ lit: "L", enabled: ["L"] });
    expect(markerColour()).toBe(colourOf["L"]);
  });
});

// Before the engine's first frame, the row follows the settings: the source chosen is lit, the
// views shown are offered, and on a mono track L alone, which is all it draws.
describe("the trigger source before the first frame", () => {
  test("lights the source chosen and offers the views shown", async () => {
    await startBeforeFrames();
    await userEvent.click(choice("time-base", "Trig"));
    expect(window.orizonDebug?.frameCount ?? 0).toBe(0);
    await press(choice("source", "R"));
    await expect
      .element(choice("source", "R"))
      .toHaveAttribute("aria-pressed", "true");
    expect((await sourceRow()).enabled).toEqual(["L", "R"]);
    await showViews(["L", "S"]);
    expect(window.orizonDebug?.frameCount ?? 0).toBe(0);
    expect((await sourceRow()).enabled).toEqual(["L", "S"]);
  });

  test("offers L alone on a mono track", async () => {
    await startBeforeFrames({ inputChannels: 1 });
    await userEvent.click(choice("time-base", "Trig"));
    expect(window.orizonDebug?.frameCount ?? 0).toBe(0);
    expect((await sourceRow()).enabled).toEqual(["L"]);
  });
});
