// The channel the Meter, snapping and Hover read is chosen in the Views popover's Measure row, beside
// the views it can read (DESIGN.md 1.2.1): only the views drawn can be chosen, the one read is lit,
// and the Views field names both, the views drawn lit, then a dot and the channel read. Multi reads
// this track's lane channel and has no Measure row.
import { describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import {
  byId,
  channelRead,
  displays,
  drawnLetters,
  settle,
  showDisplay,
  showViews,
  startApp,
  startBeforeFrames,
  useReadout,
  rowHidden,
} from "./harness.ts";

const letters = ["L", "R", "M", "S"] as const;
const named = { L: "Left", R: "Right", M: "Mid", S: "Side" } as const;

// The Measure row's choices: which can be chosen and which is lit.
function readsRow(): { enabled: string[]; lit: string[] } {
  const buttons = [...byId("readout-source").querySelectorAll("button")];
  return {
    enabled: buttons.filter((b) => !b.disabled).map((b) => b.textContent),
    lit: buttons
      .filter((b) => b.getAttribute("aria-pressed") === "true")
      .map((b) => b.textContent),
  };
}

const summaryWidth = () => byId("views-single").getBoundingClientRect().width;

async function openViews(): Promise<void> {
  if (byId("views-popover").hidden) {
    await userEvent.click(byId("views-details"));
  }
  await expect.element(byId("views-popover")).toBeVisible();
}

describe("the Measure row", () => {
  test("sits in the Views popover, under Channels, and leaves the status bar", async () => {
    await startApp();
    await settle();
    await openViews();
    const rows = [
      ...byId("views-popover").querySelectorAll<HTMLElement>(".popover-row"),
    ].filter((row) => !row.hidden);
    expect(rows.map((row) => row.querySelector("span")?.textContent)).toEqual([
      "Channels",
      "Measure",
    ]);
    expect(rows[1]?.contains(byId("readout-source"))).toBe(true);
    expect(document.querySelector(".statusbar #readout-source")).toBeNull();
  });

  // Each view hidden in turn: it cannot be chosen, and while it was the one chosen, the first view
  // drawn is read and lit instead; drawn again, it is read again.
  test.each(letters)(
    "offers only the views drawn, and reads the first drawn while %s is hidden",
    async (hidden) => {
      await startApp();
      await settle();
      await showViews(letters);
      await useReadout(hidden);
      await expect.poll(channelRead).toBe(named[hidden]);
      const drawn = letters.filter((letter) => letter !== hidden);
      await showViews(drawn);
      await expect.poll(channelRead).toBe(named[drawn[0] ?? "L"]);
      await openViews();
      expect(readsRow()).toEqual({ enabled: drawn, lit: [drawn[0]] });
      await userEvent.click(byId("views-details"));
      await showViews(letters);
      await expect.poll(channelRead).toBe(named[hidden]);
    },
  );

  test("offers and reads Left alone on a mono track", async () => {
    await startApp({ inputChannels: 1 });
    await settle();
    await expect.poll(channelRead).toBe("Left");
    await openViews();
    expect(readsRow()).toEqual({ enabled: ["L"], lit: ["L"] });
  });

  // Before a frame says what is read, nothing can be chosen and the field reads "—"; the first
  // frame names the channel, at the same width.
  test("offers nothing and reads a dash before the first frame", async () => {
    const app = await startBeforeFrames();
    await openViews();
    expect(readsRow()).toEqual({ enabled: [], lit: [] });
    expect(channelRead()).toBe("—");
    const before = summaryWidth();
    app.holdFrames(false);
    await expect.poll(channelRead).toBe("Left");
    expect(readsRow()).toEqual({ enabled: ["L", "R"], lit: ["L"] });
    expect(summaryWidth()).toBeCloseTo(before, 1);
  });

  // Multi reads this track's lane channel: Single's Measure row is hidden there, and Multi's own,
  // of the same name and in the same place, chooses the lane channel.
  test.each(displays.filter((display) => display !== "Single"))(
    "is the lane channel's row in %s",
    async (display) => {
      await startApp();
      await settle();
      await showDisplay(display);
      await settle();
      expect(rowHidden("readout-source")).toBe(true);
      expect(rowHidden("lane-channel")).toBe(false);
      await openViews();
      const rows = [
        ...byId("views-popover").querySelectorAll<HTMLElement>(".popover-row"),
      ].filter((row) => !row.hidden);
      expect(rows[1]?.querySelector("span")?.textContent).toBe("Measure");
      expect(rows[1]?.contains(byId("lane-channel"))).toBe(true);
    },
  );
});

describe("the Views field", () => {
  // How many views are drawn of the four, at one width however many; its name for a screen reader
  // says the channel read too.
  test("counts the views drawn, at one width, and names the channel read to a screen reader", async () => {
    await startApp();
    await settle();
    await showViews(["L", "R", "M"]);
    const widths = new Set<number>();
    for (const letter of ["L", "R", "M"] as const) {
      await useReadout(letter);
      await expect.poll(channelRead).toBe(named[letter]);
      expect(drawnLetters()).toEqual(["L", "R", "M"]);
      expect(byId("views-shown").textContent).toBe("3 of 4");
      expect(byId("views-details").getAttribute("aria-label")).toBe(
        `Views: 3 of 4 channels, measures ${named[letter]}`,
      );
      widths.add(Math.round(summaryWidth() * 100));
    }
    await showViews(["S"]);
    await expect.poll(channelRead).toBe("Side");
    expect(byId("views-shown").textContent).toBe("1 of 4");
    widths.add(Math.round(summaryWidth() * 100));
    expect(widths.size).toBe(1);
  });

  test("names a mono track's Left, at the stereo width", async () => {
    await startApp();
    await settle();
    const stereo = summaryWidth();
    await startApp({ inputChannels: 1 });
    await settle();
    await expect.poll(channelRead).toBe("Left");
    expect(drawnLetters()).toEqual(["L"]);
    expect(summaryWidth()).toBeCloseTo(stereo, 1);
  });
});
