// One Views field and popover for every display (#412): its rows follow the mode, those that do
// not apply hidden; the field's help follows the mode; a mode change closes the popover, a choice
// made in it does not. A4 is chosen in the Grid popover (#425, grid-popover.test.ts).
import { describe, expect, test, vi } from "vitest";
import { userEvent } from "vitest/browser";

import { helpTexts } from "../../src/view/help.ts";
import {
  byId,
  choice,
  displays,
  showDisplay,
  startApp,
  type Display,
} from "./harness.ts";

// The rows each display's Views popover shows, in order.
const rows: Readonly<Record<Display, readonly string[]>> = {
  Single: ["Channels", "Measure"],
  Multi: ["Channels", "Measure", "Compare"],
  Compare: ["Channels", "Measure", "Compare"],
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

  test("the Views popover shows the display's rows and hides the others", async () => {
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
  });

  // With help on, the field explains the display's own settings.
  test("explains the Views field with the display's help", async () => {
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
      helpTexts[display === "Single" ? "channelViews" : "tracks"].lead,
    );
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

  test("stays open while Compare is chosen in it", async () => {
    await startApp();
    await showDisplay("Multi");
    await openViews();
    for (const [host, label] of [
      ["compare", "Sum"],
      ["compare", "Off"],
      ["compare", "Diff"],
    ] as const) {
      const button = choice(host, label);
      await userEvent.click(button);
      await expect.element(button).toHaveAttribute("aria-pressed", "true");
      await expect.element(byId("views-popover")).toBeVisible();
    }
  });
});
