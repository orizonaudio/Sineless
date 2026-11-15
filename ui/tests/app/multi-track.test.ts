// The multi-track view with Compare off and on, through the real page: lane labels from the
// engine's lanes messages, the trigger marker in this track's lane, the Compare switch and its
// readouts over the tracks ticked in the Views popover's Channels list.
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import {
  beforeApp,
  byId,
  choice,
  freezeLabel,
  press,
  setCompare,
  settle,
  showOnly,
  startApp,
  type Started,
  tickedTracks,
  multiSummary,
} from "./harness.ts";

function laneNames(): string[] {
  return [...byId("lane-labels").querySelectorAll(".lane-label")].map(
    (label) => label.textContent,
  );
}

// The match readout after a track's name on its lane label, or undefined without one.
function matchOf(name: string): string | undefined {
  const label = [
    ...byId("lane-labels").querySelectorAll<HTMLElement>(".lane-label > span"),
  ].find((chip) => chip.textContent.startsWith(name));
  return label?.querySelector(".lane-readout")?.textContent;
}

const everyOther = ["Bass", "Track 3", "Kick 2"];

// Multi with Compare off or on, Seconds, stacked, with R as the Single view's trigger source: R's
// lane is not the first, so a marker placed by the views rather than the tracks would show. It
// shows this track and `others`: every other track unless given.
async function show(
  mode: "Multi" | "Compare",
  others: readonly string[] = everyOther,
): Promise<void> {
  await press(choice("time-base", "Trig"));
  await press(choice("display-mode", "Single"));
  await press(choice("source", "R"));
  await press(choice("display-mode", "Multi"));
  await showOnly(others);
  await setCompare(mode === "Compare");
  await press(choice("layout", "Stack"));
  await settle();
}

function hasSumLane(): boolean {
  return laneNames().some((name) => name.startsWith("Sum"));
}

function previewFix(): HTMLElement | null {
  return byId("lane-labels").querySelector<HTMLElement>(".preview-fix");
}

let started: Started;

beforeApp(async () => {
  started = await startApp();
});

describe("the multi-track view", () => {
  test("labels each lane with its track, and stacks them", async () => {
    await show("Multi");
    expect(window.orizonDebug?.lastFrame?.lanes).toHaveLength(4);
    // The hi-hats have no name: their lane is named by its place.
    await expect.poll(laneNames).toEqual(["Kick", "Bass", "Track 3", "Kick 2"]);
  });

  test("puts the trigger marker in this track's lane", async () => {
    await show("Multi");
    // Kick, this instance's track, is the first of four stacked lanes; R would be the second.
    const screen = byId("screen").getBoundingClientRect();
    const marker = byId("trigger-marker").getBoundingClientRect();
    const y = (marker.top + marker.height / 2 - screen.top) / screen.height;
    expect(y).toBeGreaterThan(0);
    expect(y).toBeLessThan(0.25);
  });
});

describe("the Compare view", () => {
  test("adds the sum of every track shown, each measured against this one", async () => {
    await show("Compare");
    await expect
      .poll(() => laneNames().some((name) => name.startsWith("Sum")))
      .toBe(true);
    // No track is picked for the comparison: the swatches are swatches.
    expect(byId("lane-labels").querySelector("[role='checkbox']")).toBeNull();
    await expect.poll(() => matchOf("Kick 2")).toBe("+1.3 ms Ø · −0.93");
    expect(matchOf("Bass")).toBeDefined();
    expect(matchOf("Track 3")).toBeDefined();
    expect(matchOf("Kick"), "this track is the reference").toBeUndefined();
  });

  test("finds Kick 2 late and inverted, and the fix lifts the sum", async () => {
    await show("Compare", ["Kick 2"]);
    await settle();
    // 62 samples at 48 kHz, as the mock delays it 1.3 ms.
    await expect.poll(() => matchOf("Kick 2")).toBe("+1.3 ms Ø · −0.93");
    const sum = () =>
      byId("lane-labels").querySelector<HTMLElement>("[data-help='sumGain']");
    await expect.poll(() => sum()?.classList.contains("cancels")).toBe(true);

    await userEvent.click(previewFix() as HTMLElement);
    await settle();
    await expect.poll(() => sum()?.classList.contains("reinforces")).toBe(true);
    // The label is built again for each state: the button now on it.
    await userEvent.click(previewFix() as HTMLElement);
  });

  // The kick and the bass have nothing in common: together they are as loud as the two apart,
  // and the sum's gain is marked neither as a loss nor as a lift.
  test("marks a sum that neither cancels nor reinforces as neither", async () => {
    await show("Compare", ["Bass"]);
    await settle();
    const sum = () =>
      byId("lane-labels").querySelector<HTMLElement>("[data-help='sumGain']");
    await expect.poll(() => sum()?.textContent).toMatch(/dB$/u);
    expect(sum()?.classList.contains("cancels")).toBe(false);
    expect(sum()?.classList.contains("reinforces")).toBe(false);
  });

  // The tracks shown are the instance's: the engine keeps them, and the page reopened shows
  // them, without asking for any.
  test("keeps the ticked tracks when the editor reopens", async () => {
    await show("Compare", ["Track 3", "Kick 2"]);
    started = await startApp({}, started.transport);
    await expect.poll(tickedTracks).toEqual(["Kick", "Track 3", "Kick 2"]);
    const known = ["Kick 2", "Track 3", "Kick", "Sum"];
    await expect
      .poll(() =>
        laneNames().map((name) => known.find((each) => name.startsWith(each))),
      )
      .toEqual(["Kick", "Track 3", "Kick 2", "Sum"]);
    expect(started.sent.some((message) => message.type === "showTracks")).toBe(
      false,
    );
  });

  test("says when no other track is shown, and where to tick one", async () => {
    await show("Compare", []);
    await settle();
    await expect
      .poll(
        () => byId("lane-labels").querySelector(".compare-note")?.textContent,
      )
      .toBe(
        "No other track to compare with. Tick one under Channels, in Views.",
      );
  });
});

describe("the Compare switch", () => {
  // Off, Multi shows the tracks alone: no readouts, no sum, no note, and nothing compared.
  test("adds nothing to Multi while it is off", async () => {
    await show("Multi");
    expect(byId("lane-labels").querySelector(".lane-readout")).toBeNull();
    expect(hasSumLane()).toBe(false);
    await showOnly([]);
    await settle();
    expect(byId("lane-labels").querySelector(".compare-note")).toBeNull();
    expect(window.orizonDebug?.lastFrame?.comparison).toBeUndefined();
  });

  // Left on in Single, it shows nothing there; back in Multi, Compare shows again, as the Views
  // field says.
  test("keeps its state in Single and shows Compare back in Multi", async () => {
    await show("Compare");
    await expect.poll(hasSumLane).toBe(true);
    await press(choice("display-mode", "Single"));
    await settle();
    expect(hasSumLane()).toBe(false);
    expect(window.orizonDebug?.lastFrame?.comparison).toBeUndefined();
    await press(choice("display-mode", "Multi"));
    await expect.poll(hasSumLane).toBe(true);
    expect(multiSummary()).toBe("4/4·M··Σ");
  });

  // While Compare stays on screen, Preview fix stays on: pressing the Multi or On already pressed,
  // or the engine sending those settings again unchanged (it answers every request, and resends
  // all of them when the audio format changes), leaves it as the user set it.
  test("keeps Preview fix on while Compare stays on screen", async () => {
    await show("Compare");
    await expect.poll(previewFix).not.toBeNull();
    await userEvent.click(previewFix() as HTMLElement);
    await expect
      .element(previewFix() as HTMLElement)
      .toHaveAttribute("aria-pressed", "true");
    const sent = started.sent.length;
    await press(choice("display-mode", "Multi"));
    await setCompare(true);
    await press(choice("compare", "Sum"));
    started.deliver({
      type: "stateChanged",
      name: "displayMode",
      value: "multi",
    });
    started.deliver({ type: "stateChanged", name: "compare", value: "on" });
    await settle();
    expect(
      started.sent
        .slice(sent)
        .filter((message) => message.type === "previewFix"),
    ).toEqual([]);
    expect(previewFix()?.getAttribute("aria-pressed")).toBe("true");
    await userEvent.click(previewFix() as HTMLElement);
  });

  // Preview fix is Compare's: turning Compare off, or leaving Multi, resets it, and Compare comes
  // back without it.
  test.each(["switch", "display mode"])(
    "resets Preview fix when Compare leaves the screen by the %s",
    async (how) => {
      await show("Compare");
      await expect.poll(previewFix).not.toBeNull();
      await userEvent.click(previewFix() as HTMLElement);
      await expect
        .element(previewFix() as HTMLElement)
        .toHaveAttribute("aria-pressed", "true");
      const sent = started.sent.length;
      if (how === "switch") {
        await setCompare(false);
      } else {
        await press(choice("display-mode", "Single"));
      }
      await expect
        .poll(() =>
          started.sent
            .slice(sent)
            .some((message) => message.type === "previewFix" && !message.on),
        )
        .toBe(true);
      if (how === "switch") {
        await setCompare(true);
      } else {
        await press(choice("display-mode", "Multi"));
      }
      await expect.poll(previewFix).not.toBeNull();
      expect(previewFix()?.getAttribute("aria-pressed")).toBe("false");
    },
  );
});

// This track shown alone: Compare adds no lane, so its frames' lanes stay as they were. The note
// must still follow the switch at once, live or frozen, in either layout.
describe("a track shown alone", () => {
  const note = () =>
    byId("lane-labels").querySelector(".compare-note")?.textContent ?? null;

  async function alone(layout: string, frozen: boolean): Promise<void> {
    await show("Multi", []);
    await press(choice("layout", layout));
    await settle();
    await expect.poll(laneNames).toHaveLength(1);
    // The file's tests share one page (beforeApp), which an earlier test may have left frozen:
    // press Freeze only when the page is not already as this test needs it. The frozen frame can
    // take longer than the matcher's default second in a busy browser.
    const wanted = frozen ? "Frozen" : "Freeze";
    if (freezeLabel() !== wanted) {
      await userEvent.click(byId("freeze"));
    }
    await expect.poll(() => freezeLabel(), { timeout: 5000 }).toBe(wanted);
  }

  describe.each(["Stack", "Overlay"])("%s", (layout) => {
    test.each([
      ["live", false],
      ["frozen", true],
    ])(
      "says there is no other track as soon as Compare is on, %s, and stops when it is off",
      async (_, frozen) => {
        await alone(layout, frozen);
        expect(note()).toBeNull();
        await setCompare(true);
        await expect.poll(note).toContain("No other track to compare with");
        await setCompare(false);
        await expect.poll(note).toBeNull();
      },
    );

    test("says it again on coming back to Multi from Single with Compare on", async () => {
      await alone(layout, false);
      await setCompare(true);
      await expect.poll(note).toContain("No other track to compare with");
      await press(choice("display-mode", "Single"));
      await expect.poll(note).toBeNull();
      await press(choice("display-mode", "Multi"));
      await expect.poll(note).toContain("No other track to compare with");
    });
  });
});

// The only instance in the host: nothing to tick, so the note says what to do about it.
describe("the only track with this plugin", () => {
  test("says to insert the plugin on another track when Compare is on", async () => {
    started = await startApp({ trackCount: 1 });
    await press(choice("display-mode", "Multi"));
    await setCompare(true);
    await expect
      .poll(
        () => byId("lane-labels").querySelector(".compare-note")?.textContent,
      )
      .toBe(
        "No other track to compare with. Insert Sineless on another track.", // The branded name (vitest.config.ts).
      );
  });
});
