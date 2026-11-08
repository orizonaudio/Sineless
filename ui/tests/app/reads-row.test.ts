// The channel the Meter, snapping and Hover read is chosen in the Views popover's Reads row, beside
// the views it can read (DESIGN.md 1.2.1): only the views drawn can be chosen, the one read is lit,
// and the Views field names both, the views drawn lit, then a dot and the channel read. Multi reads
// this track's Mid and has no Reads row.
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
} from "./harness.ts";

const letters = ["L", "R", "M", "S"] as const;
const named = { L: "Left", R: "Right", M: "Mid", S: "Side" } as const;

// The Reads row's choices: which can be chosen and which is lit.
function readsRow(): { enabled: string[]; lit: string[] } {
  const buttons = [...byId("readout-source").querySelectorAll("button")];
  return {
    enabled: buttons.filter((b) => !b.disabled).map((b) => b.textContent),
    lit: buttons
      .filter((b) => b.getAttribute("aria-pressed") === "true")
      .map((b) => b.textContent),
  };
}

// The read part of the Views field: the letter shown, and how opaque it is on screen, its own
// opacity times its host's.
function readLetter(): { text: string; opacity: number } {
  const host = byId("views-read");
  const shown = host.querySelector(":scope > .on");
  const opacity = (element: Element) =>
    Number(getComputedStyle(element).opacity);
  return {
    text: shown?.textContent ?? "",
    opacity: shown === null ? 0 : opacity(shown) * opacity(host),
  };
}

const summaryWidth = () => byId("views-summary").getBoundingClientRect().width;

async function openViews(): Promise<void> {
  if (byId("views-popover").hidden) {
    await userEvent.click(byId("views-details"));
  }
  await expect.element(byId("views-popover")).toBeVisible();
}

describe("the Reads row", () => {
  test("sits in the Views popover, under Channels, and leaves the status bar", async () => {
    await startApp();
    await settle();
    await openViews();
    const rows = [...byId("views-popover").querySelectorAll(".popover-row")];
    expect(rows.map((row) => row.querySelector("span")?.textContent)).toEqual([
      "Channels",
      "Reads",
    ]);
    expect(rows[1]?.contains(byId("readout-source"))).toBe(true);
    expect(document.querySelector(".statusbar #readout-source")).toBeNull();
    expect(document.getElementById("readout-details")).toBeNull();
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
    expect(readLetter().text).toBe("—");
    expect(channelRead()).toBe("—");
    const before = summaryWidth();
    app.holdFrames(false);
    await expect.poll(channelRead).toBe("Left");
    expect(readsRow()).toEqual({ enabled: ["L", "R"], lit: ["L"] });
    expect(summaryWidth()).toBeCloseTo(before, 1);
  });

  // Multi reads this track's Mid: the Views field, and the Reads row with it, is out of reach, and
  // nothing else offers a channel.
  test.each(displays.filter((display) => display !== "Single"))(
    "is out of reach in %s",
    async (display) => {
      await startApp();
      await settle();
      await showDisplay(display);
      await settle();
      expect(byId("views-group").inert).toBe(true);
      expect(byId("views-popover").hidden).toBe(true);
      expect(document.getElementById("readout-details")).toBeNull();
    },
  );
});

describe("the Views field", () => {
  // The views drawn lit, then a dot and the channel read, lit too; one width whichever is read.
  test("names the views drawn and the channel read, at one width", async () => {
    await startApp();
    await settle();
    await showViews(["L", "R", "M"]);
    const widths = new Set<number>();
    for (const letter of ["L", "R", "M"] as const) {
      await useReadout(letter);
      await expect.poll(channelRead).toBe(named[letter]);
      expect(drawnLetters()).toEqual(["L", "R", "M"]);
      expect(byId("views-summary").textContent.startsWith("LRMS·")).toBe(true);
      expect(readLetter()).toEqual({ text: letter, opacity: 1 });
      expect(byId("views-details").getAttribute("aria-label")).toBe(
        `Views: L R M, reads ${named[letter]}`,
      );
      widths.add(Math.round(summaryWidth() * 100));
    }
    // A view not drawn stays in its place, dimmed.
    const side = byId("views-summary").querySelector(
      ":scope > span:nth-child(4)",
    );
    expect(side?.textContent).toBe("S");
    expect(
      Number(getComputedStyle(side ?? document.body).opacity),
    ).toBeLessThan(1);
    await showViews(["S"]);
    await expect.poll(channelRead).toBe("Side");
    expect(readLetter()).toEqual({ text: "S", opacity: 1 });
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
    expect(readLetter()).toEqual({ text: "L", opacity: 1 });
    expect(summaryWidth()).toBeCloseTo(stereo, 1);
  });
});
