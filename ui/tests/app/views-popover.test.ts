// One Views field and popover for every display (#412): its rows follow the mode, those that do
// not apply hidden, with A4 last in each; A4 there tunes the note names the readouts show; the
// field's help follows the mode; a mode change closes the popover, a choice made in it does not.
import { describe, expect, test, vi } from "vitest";
import { userEvent } from "vitest/browser";

import { helpTexts } from "../../src/view/help.ts";
import {
  byId,
  choice,
  cursorRow,
  displays,
  inTimeBase,
  setPressed,
  settle,
  showDisplay,
  startApp,
  typeInto,
  type Display,
} from "./harness.ts";

// The rows each display's Views popover shows, in order.
const rows: Readonly<Record<Display, readonly string[]>> = {
  Single: ["Channels", "Measure", "A4"],
  Multi: ["Group", "Channel", "Compare", "Labels", "A4"],
  Compare: ["Group", "Channel", "Compare", "Labels", "A4"],
};

function shownRows(): string[] {
  return [
    ...byId("views-popover").querySelectorAll<HTMLElement>(".popover-row"),
  ]
    .filter((row) => row.checkVisibility())
    .map((row) => row.querySelector("span")?.textContent ?? "");
}

// The pointer over an element's centre, as help's tooltip follows it.
function hover(id: string): void {
  const box = byId(id).getBoundingClientRect();
  byId(id).dispatchEvent(
    new PointerEvent("pointermove", {
      pointerId: 1,
      bubbles: true,
      clientX: box.left + box.width / 2,
      clientY: box.top + box.height / 2,
    }),
  );
}

async function openViews(): Promise<void> {
  if (byId("views-popover").hidden) {
    await userEvent.click(byId("views-details"));
  }
  await expect.element(byId("views-popover")).toBeVisible();
}

describe.each(displays)("in %s", (display) => {
  // Both modes' summaries share one place in the field: only the display's own is drawn, so the
  // field is never blank nor two summaries over each other.
  test("the Views field shows the display's summary alone", async () => {
    await startApp();
    await showDisplay(display);
    const drawn = (id: string) => getComputedStyle(byId(id)).visibility;
    expect(drawn("views-single")).toBe(
      display === "Single" ? "visible" : "hidden",
    );
    expect(drawn("views-multi")).toBe(
      display === "Single" ? "hidden" : "visible",
    );
  });

  test("the Views popover shows the display's rows, then A4, and hides the others", async () => {
    await startApp();
    await showDisplay(display);
    await openViews();
    expect(shownRows()).toEqual(rows[display]);
    // The rows of the other mode are out of sight and out of the accessibility tree.
    for (const row of byId("views-popover").querySelectorAll<HTMLElement>(
      ".popover-row",
    )) {
      if (!row.checkVisibility()) {
        expect(row.closest("[hidden]"), row.textContent).not.toBeNull();
      }
    }
    // With Compare on, Labels shows but cannot be chosen: Compare's labels always show.
    if (display === "Compare") {
      expect(choice("lane-labels-choice", "Off").disabled).toBe(true);
    }
  });

  // With help on, the field explains the display's own settings, and the A4 row concert pitch.
  test("explains the Views field and its A4 row with the display's help", async () => {
    await startApp();
    await showDisplay(display);
    await userEvent.click(byId("help"));
    await expect.element(byId("help")).toHaveAttribute("aria-pressed", "true");
    const explains = async (id: string, lead: string) => {
      hover(id);
      await vi.waitFor(
        () => {
          expect(byId("help-tip").textContent).toContain(lead);
        },
        { timeout: 5000 },
      );
    };
    await explains(
      "views-details",
      helpTexts[display === "Single" ? "channelViews" : "group"].lead,
    );
    await openViews();
    await explains("concert-pitch-field", helpTexts.concertPitch.lead);
  });
});

describe.each(["Single", "Multi"] as const)("A4 in %s", (mode) => {
  test("is chosen in the Views popover and tunes the time cursors' note name", async () => {
    const app = await startApp();
    await showDisplay(mode);
    // A 10 ms span in Trig puts the time cursors a third of it apart, at 300 Hz: audible, so named.
    await inTimeBase("Trig");
    await typeInto("span-field", "10 ms");
    await settle();
    await setPressed(byId("time-cursors"), true);
    await expect.poll(() => cursorRow("Note")).not.toBe("");
    const at440 = cursorRow("Note");
    await openViews();
    expect(byId("views-popover").contains(byId("concert-pitch-field"))).toBe(
      true,
    );
    await typeInto("concert-pitch-field", "432");
    expect(
      app.sent.filter((message) => message.type === "setSetting").at(-1),
    ).toEqual({ type: "setSetting", name: "concertPitch", value: 432 });
    // A lower A4 names the same frequency sharper.
    await expect.poll(() => cursorRow("Note")).not.toBe(at440);
    await typeInto("concert-pitch-field", "440");
    await expect.poll(() => cursorRow("Note")).toBe(at440);
  });
});

describe("the Views popover", () => {
  test.each([
    ["Multi", "single"],
    ["Single", "multi"],
  ] as const)(
    "closes when the display mode changes from %s",
    async (from, to) => {
      const app = await startApp();
      await showDisplay(from);
      await openViews();
      // As a host's undo or a restored project would change it.
      app.transport.send({
        type: "setSetting",
        name: "displayMode",
        value: to,
      });
      await expect.element(byId("views-popover")).not.toBeVisible();
      expect(byId("views-details").getAttribute("aria-expanded")).toBe("false");
    },
  );

  test("stays open while Compare and Labels are chosen in it", async () => {
    await startApp();
    await showDisplay("Multi");
    await openViews();
    for (const [host, label] of [
      ["compare", "On"],
      ["compare", "Off"],
      ["lane-labels-choice", "Off"],
      ["lane-labels-choice", "On"],
    ] as const) {
      const button = choice(host, label);
      await userEvent.click(button);
      await expect.element(button).toHaveAttribute("aria-pressed", "true");
      await expect.element(byId("views-popover")).toBeVisible();
    }
  });
});
