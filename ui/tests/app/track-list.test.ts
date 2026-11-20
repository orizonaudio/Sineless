// Multi's Channels list in the Views popover (#425): every Sineless instance in the host, those
// ticked shown as lanes. Through the real page on the mock engine: the list as the engine sends
// it, ticking, None and All, the most tracks Multi shows, instances that come and go, and the
// count in the Views field.
import { describe, expect, test, vi } from "vitest";
import { userEvent } from "vitest/browser";

import { maxShownTracks } from "../../../protocol/src/limits.ts";
import { MockTransport } from "../../src/bridge/mock-transport.ts";
import {
  byId,
  choice,
  multiSummary,
  press,
  settle,
  startApp,
  tickedTracks,
  tickTrack,
  trackRows,
} from "./harness.ts";

function laneNames(): string[] {
  return [
    ...byId("lane-labels").querySelectorAll<HTMLElement>(".lane-label"),
  ].map(
    (label) =>
      label.querySelector<HTMLElement>(".cut-shown")?.textContent ??
      label.textContent,
  );
}

const count = () => byId("tracks").querySelector(".check-count")?.textContent;
const names = () => trackRows().map((row) => row.name);
const locked = () =>
  trackRows()
    .filter((row) => row.box.getAttribute("aria-disabled") === "true")
    .map((row) => row.name);

async function action(label: "None" | "All"): Promise<void> {
  const button = [...byId("tracks").querySelectorAll("button")].find(
    (each) => each.textContent === label,
  );
  expect(button, label).toBeDefined();
  await userEvent.click(button as HTMLElement);
}

async function openList(): Promise<void> {
  await press(choice("display-mode", "Multi"));
  await press(choice("layout", "Stack"));
  if (byId("views-popover").hidden) {
    await userEvent.click(byId("views-details"));
  }
  await expect.element(byId("tracks")).toBeVisible();
}

describe("Multi's Channels list", () => {
  test("lists every track in the host's order, this one marked and ticked for good", async () => {
    await startApp({ othersShown: false, tracksBefore: 1 });
    await openList();
    // The unnamed hi-hats are named by their place among every track.
    await expect.poll(names).toEqual(["Bass", "Kick", "Track 3", "Kick 2"]);
    expect(tickedTracks()).toEqual(["Kick"]);
    expect(count()).toBe("1 of 4 selected");
    const own = trackRows()[1];
    expect(own?.row.textContent).toContain("this track");
    expect(own?.box.getAttribute("aria-disabled")).toBe("true");
    // A new instance shows its own track alone.
    await expect.poll(laneNames).toEqual(["Kick"]);
    // This track cannot be unticked.
    own?.box.click();
    await settle();
    expect(tickedTracks()).toEqual(["Kick"]);
  });

  test("shows a track's lane when it is ticked, in the host's order, and takes it away when unticked", async () => {
    const app = await startApp({ othersShown: false });
    await openList();
    await tickTrack("Kick 2", true);
    await expect.poll(laneNames).toEqual(["Kick", "Kick 2"]);
    expect(app.sent.at(-1)).toEqual({ type: "showTracks", ids: [4] });
    await tickTrack("Bass", true);
    await expect.poll(laneNames).toEqual(["Kick", "Bass", "Kick 2"]);
    expect(count()).toBe("3 of 4 selected");
    expect(multiSummary()).toBe("3 of 4");
    await tickTrack("Kick 2", false);
    await expect.poll(laneNames).toEqual(["Kick", "Bass"]);
    // The popover stays open while tracks are chosen.
    expect(byId("views-popover").hidden).toBe(false);
  });

  test("gives a track the same colour and name in the list and on its lane, whichever are shown", async () => {
    await startApp({ othersShown: false });
    await openList();
    await tickTrack("Track 3", true);
    await expect.poll(laneNames).toEqual(["Kick", "Track 3"]);
    const row = trackRows().find((each) => each.name === "Track 3");
    const swatch = row?.row.querySelector<HTMLElement>(".check-swatch");
    const chip = [
      ...byId("lane-labels").querySelectorAll<HTMLElement>(
        ".lane-label > span",
      ),
    ][1];
    const paint = document.createElement("span");
    paint.style.color = chip?.style.getPropertyValue("--swatch") ?? "";
    document.body.append(paint);
    const onLane = getComputedStyle(paint).color;
    paint.remove();
    expect(getComputedStyle(swatch as HTMLElement).backgroundColor).toBe(
      onLane,
    );
  });

  test("None leaves this track alone, and All ticks every track", async () => {
    await startApp();
    await openList();
    await expect.poll(tickedTracks).toHaveLength(4);
    await action("None");
    await expect.poll(tickedTracks).toEqual(["Kick"]);
    await expect.poll(laneNames).toEqual(["Kick"]);
    expect(count()).toBe("1 of 4 selected");
    await action("All");
    await expect.poll(tickedTracks).toHaveLength(4);
    await expect.poll(laneNames).toHaveLength(4);
    expect(count()).toBe("4 of 4 selected");
  });

  test("ticks with the keyboard, the box keeping the focus as the list changes", async () => {
    await startApp({ othersShown: false });
    await openList();
    const bass = () => trackRows().find((row) => row.name === "Bass");
    bass()?.box.focus();
    await userEvent.keyboard(" ");
    await expect.poll(() => bass()?.box.checked).toBe(true);
    expect(document.activeElement).toBe(bass()?.box);
    await userEvent.keyboard(" ");
    await expect.poll(() => bass()?.box.checked).toBe(false);
    expect(document.activeElement).toBe(bass()?.box);
  });

  test("names the group and says how many are ticked, for a screen reader", async () => {
    await startApp();
    await openList();
    const list = byId("tracks");
    expect(list.getAttribute("role")).toBe("group");
    expect(byId(list.getAttribute("aria-labelledby") ?? "").textContent).toBe(
      "Channels",
    );
    expect(list.querySelector(".check-count")?.getAttribute("role")).toBe(
      "status",
    );
    expect(trackRows()[0]?.box.getAttribute("aria-description")).toBe(
      "This track is always shown",
    );
    expect(byId("views-details").getAttribute("aria-label")).toBe(
      "Views: 4 of 4 tracks, Mid",
    );
  });
});

// Twenty instances: Multi shows this track and fourteen others, so that Compare's lane always
// has a place.
describe("with more tracks than Multi shows", () => {
  test("stops at fifteen ticked: the rest cannot be ticked, and say why, until one is unticked", async () => {
    await startApp({ trackCount: 20, othersShown: false });
    await openList();
    await expect.poll(names).toHaveLength(20);
    await action("All");
    await expect.poll(tickedTracks).toHaveLength(maxShownTracks);
    // This track and the first fourteen others, in the host's order.
    expect(tickedTracks()).toEqual(names().slice(0, maxShownTracks));
    expect(count()).toBe(`${String(maxShownTracks)} of 20 selected`);
    const rest = names().slice(maxShownTracks);
    expect(locked()).toEqual(["Kick", ...rest]);
    const last = trackRows().at(-1);
    expect(last?.box.getAttribute("aria-description")).toBe(
      "At most 15 tracks are shown",
    );
    // It stays in the tab order, so that it can be found, and takes no tick.
    expect(last?.box.disabled).toBe(false);
    last?.box.click();
    await settle();
    expect(last?.box.checked).toBe(false);
    expect(window.orizonDebug?.lastFrame?.lanes).toHaveLength(maxShownTracks);

    // One unticked frees a place.
    await tickTrack("Bass", false);
    expect(locked()).toEqual(["Kick"]);
    await tickTrack(names().at(-1) ?? "", true);
    await expect.poll(tickedTracks).toHaveLength(maxShownTracks);

    // Full again, with an unticked row before a ticked one: a click on it changes nothing, where
    // taking it would push the last track shown off the screen.
    const before = tickedTracks();
    expect(locked()).toContain("Bass");
    trackRows()
      .find((row) => row.name === "Bass")
      ?.box.click();
    await settle();
    expect(tickedTracks()).toEqual(before);
  });

  test("keeps every track on screen when Compare comes on, in its own sixteenth lane", async () => {
    await startApp({ trackCount: 20 });
    await openList();
    await expect.poll(tickedTracks).toHaveLength(maxShownTracks);
    await press(choice("compare", "Sum"));
    await expect
      .poll(() => window.orizonDebug?.lastFrame?.lanes.length)
      .toBe(maxShownTracks + 1);
    expect(tickedTracks()).toHaveLength(maxShownTracks);
  });

  // The count has one digit or two each side: the field keeps one width and the chevron its
  // place, a short count centred in it as a short value is in the other fields.
  test("keeps the Views field at one width whatever the count", async () => {
    await startApp({ trackCount: 20, othersShown: false });
    await openList();
    const field = () => byId("views-details").getBoundingClientRect().width;
    const chevron = () =>
      byId("views-details").querySelector(".chevron")?.getBoundingClientRect()
        .left;
    expect(multiSummary()).toBe("1 of 20");
    const width = field();
    const place = chevron();
    await action("All");
    await expect.poll(multiSummary).toBe("15 of 20");
    expect(field()).toBeCloseTo(width, 1);
    expect(chevron()).toBeCloseTo(place ?? NaN, 1);
    const centre = (id: string) => {
      const box = byId(id).getBoundingClientRect();
      return box.left + box.width / 2;
    };
    await action("None");
    await expect.poll(multiSummary).toBe("1 of 20");
    const text = document.createRange();
    text.selectNodeContents(byId("views-count"));
    const shown = text.getBoundingClientRect();
    expect(shown.left + shown.width / 2).toBeCloseTo(centre("views-multi"), 0);

    // Nine tracks, as a small session has: a digit each side, at the same width.
    await startApp({ trackCount: 9 });
    await openList();
    expect(multiSummary()).toBe("9 of 9");
    expect(field()).toBeCloseTo(width, 1);
  });

  test("is four rows tall and scrolls, at one width whatever it lists", async () => {
    await startApp({ trackCount: 20 });
    await openList();
    const rows = byId("tracks").querySelector<HTMLElement>(".check-rows");
    const row = trackRows()[0]?.row.getBoundingClientRect();
    expect(rows?.clientHeight).toBeCloseTo(4 * (row?.height ?? 0) + 3, 0);
    expect(rows?.scrollHeight).toBeGreaterThan(rows?.clientHeight ?? 0);
    const width = byId("tracks").getBoundingClientRect().width;
    const popover = byId("views-popover").getBoundingClientRect().width;

    await startApp({
      names: { 2: "Lead Vocal Double (Tuned, Printed) Take Seventeen Final" },
    });
    await openList();
    expect(byId("tracks").getBoundingClientRect().width).toBe(width);
    expect(byId("views-popover").getBoundingClientRect().width).toBe(popover);
    const fourRows = byId("tracks").querySelector<HTMLElement>(".check-rows");
    expect(fourRows?.scrollHeight).toBe(fourRows?.clientHeight);
  });
});

describe("instances that come and go", () => {
  test("lists a track added to the host unticked, and drops one removed from the list and the lanes", async () => {
    const transport = new MockTransport();
    await startApp({}, transport);
    await openList();
    await expect.poll(laneNames).toHaveLength(4);
    transport.addTrack("Vox");
    await expect
      .poll(names)
      .toEqual(["Kick", "Bass", "Track 3", "Kick 2", "Vox"]);
    expect(tickedTracks()).not.toContain("Vox");
    expect(count()).toBe("4 of 5 selected");
    expect(multiSummary()).toBe("4 of 5");
    await expect.poll(laneNames).toHaveLength(4);

    transport.removeTrack(2);
    // The unnamed track is the host's second now, in the list and on its lane.
    await expect.poll(names).toEqual(["Kick", "Track 2", "Kick 2", "Vox"]);
    await expect.poll(laneNames).toEqual(["Kick", "Track 2", "Kick 2"]);
    expect(count()).toBe("3 of 4 selected");
  });

  test("keeps the tracks ticked when the editor is closed and opened, and through Single", async () => {
    const transport = new MockTransport({ othersShown: false });
    await startApp({}, transport);
    await openList();
    await tickTrack("Kick 2", true);
    await press(choice("display-mode", "Single"));
    await press(choice("display-mode", "Multi"));
    await expect.poll(laneNames).toEqual(["Kick", "Kick 2"]);

    const reopened = await startApp({}, transport);
    await openList();
    await expect.poll(tickedTracks).toEqual(["Kick", "Kick 2"]);
    await expect.poll(laneNames).toEqual(["Kick", "Kick 2"]);
    expect(reopened.sent.some((message) => message.type === "showTracks")).toBe(
      false,
    );
  });
});

// Single's Channels: the same list, with this track's four views, so both modes look and work
// alike.
describe("Single's Channels list", () => {
  const views = () => trackRows("channels");
  const ticked = () =>
    views()
      .filter((row) => row.box.checked)
      .map((row) => row.name);
  const header = () =>
    byId("channels").querySelector(".check-count")?.textContent;
  const press2 = async (label: "None" | "All") => {
    const button = [...byId("channels").querySelectorAll("button")].find(
      (each) => each.textContent === label,
    );
    await userEvent.click(button as HTMLElement);
  };

  test("lists Left, Right, Mid and Side in their traces' colours, those drawn ticked", async () => {
    const app = await startApp();
    await userEvent.click(byId("views-details"));
    await expect.element(byId("channels")).toBeVisible();
    expect(views().map((row) => row.name)).toEqual([
      "Left",
      "Right",
      "Mid",
      "Side",
    ]);
    expect(ticked()).toEqual(["Left", "Right"]);
    expect(header()).toBe("2 of 4 selected");
    expect(byId("views-shown").textContent).toBe("2 of 4");
    const colourOf = (token: string) => {
      const probe = document.createElement("span");
      probe.style.background = `var(${token})`;
      document.body.append(probe);
      const colour = getComputedStyle(probe).backgroundColor;
      probe.remove();
      return colour;
    };
    expect(
      views().map(
        (row) =>
          getComputedStyle(
            row.row.querySelector(".check-swatch") as HTMLElement,
          ).backgroundColor,
      ),
    ).toEqual(
      ["--sl-left", "--sl-right", "--sl-mid", "--sl-side"].map(colourOf),
    );
    // Ticking asks the engine for the views, in their order.
    await userEvent.click(views()[3]?.row as HTMLElement);
    await expect.poll(ticked).toEqual(["Left", "Right", "Side"]);
    expect(
      app.sent.filter((message) => message.type === "setSetting").at(-1),
    ).toEqual({
      type: "setSetting",
      name: "channelViews",
      value: ["left", "right", "side"],
    });
    await expect.poll(() => window.orizonDebug?.lastFrame?.views.size).toBe(3);
    // The popover stays open while channels are chosen.
    expect(byId("views-popover").hidden).toBe(false);
  });

  test("None leaves Left alone, and All ticks all four", async () => {
    await startApp();
    await userEvent.click(byId("views-details"));
    await press2("All");
    await expect.poll(ticked).toEqual(["Left", "Right", "Mid", "Side"]);
    expect(header()).toBe("4 of 4 selected");
    await expect.poll(() => byId("views-shown").textContent).toBe("4 of 4");
    await press2("None");
    await expect.poll(ticked).toEqual(["Left"]);
    expect(header()).toBe("1 of 4 selected");
    await expect.poll(() => window.orizonDebug?.lastFrame?.views.size).toBe(1);
  });

  test("is as wide and as tall as Multi's list, in the same place", async () => {
    await startApp();
    await userEvent.click(byId("views-details"));
    const popover = () => byId("views-popover").getBoundingClientRect();
    const single = byId("channels").getBoundingClientRect();
    const offset = single.left - popover().left;
    await press(choice("display-mode", "Multi"));
    await userEvent.click(byId("views-details"));
    await expect.element(byId("tracks")).toBeVisible();
    const multi = byId("tracks").getBoundingClientRect();
    expect(multi.width).toBe(single.width);
    expect(multi.height).toBe(single.height);
    expect(multi.left - popover().left).toBeCloseTo(offset, 1);
  });
});

describe("a long name in the list", () => {
  const long = "Drum Bus Parallel Crush Right";

  test("is cut in the middle within its row, whole to a screen reader, and rolls under the row's pointer", async () => {
    await startApp({ names: { 2: long } });
    await openList();
    const row = () => trackRows().find((each) => each.name === long);
    await expect
      .poll(() => row()?.row.querySelector(".cut-name.cut"))
      .not.toBeNull();
    const name = row()?.row.querySelector<HTMLElement>(".cut-name");
    const shown = () => name?.querySelector(".cut-shown")?.textContent ?? "";
    const [head, tail] = shown().split("…");
    expect(long.startsWith(head ?? "-")).toBe(true);
    expect(long.endsWith(tail ?? "-")).toBe(true);
    expect(tail?.length).toBeGreaterThan(2);
    expect(name?.scrollWidth).toBeLessThanOrEqual(name?.clientWidth ?? 0);
    expect(name?.querySelector(".visually-hidden")?.textContent).toBe(long);
    const atRest = shown();
    // Anywhere on the row, not only on the name.
    await userEvent.hover(row()?.row.querySelector(".check-box") as Element);
    await expect.poll(() => name?.classList.contains("rolling")).toBe(true);
    expect(shown()).toBe(long);
    await userEvent.unhover(row()?.row as Element);
    await expect.poll(() => name?.classList.contains("rolling")).toBe(false);
    expect(shown()).toBe(atRest);
    // And with the keyboard's focus on its check box.
    row()?.box.focus();
    await expect.poll(() => name?.classList.contains("rolling")).toBe(true);
    row()?.box.blur();
    await expect.poll(() => name?.classList.contains("rolling")).toBe(false);
  });

  test("stays still for a user who asked for reduced motion, and shows whole in a tip from the keyboard and anywhere on its row", async () => {
    const realMatchMedia = window.matchMedia.bind(window);
    const reduce = vi.spyOn(window, "matchMedia").mockImplementation((query) =>
      Object.defineProperty(realMatchMedia(query), "matches", {
        value: query.includes("prefers-reduced-motion"),
      }),
    );
    try {
      await startApp({ names: { 2: long } });
      await openList();
      const row = () => trackRows().find((each) => each.name === long);
      await expect
        .poll(() => row()?.row.querySelector(".cut-name.cut"))
        .not.toBeNull();
      const name = row()?.row.querySelector<HTMLElement>(".cut-name");
      const tipShowsTheName = () =>
        vi.waitFor(
          () => {
            expect(byId("help-tip").hidden).toBe(false);
            expect(byId("help-tip").textContent).toBe(long);
          },
          { timeout: 5000 },
        );
      row()?.box.focus();
      await tipShowsTheName();
      expect(name?.classList.contains("rolling")).toBe(false);
      row()?.box.blur();
      await expect.poll(() => byId("help-tip").hidden).toBe(true);
      // The pointer on the row's check box, not on the name.
      const mark = row()?.row.querySelector(".check-box") as Element;
      const box = mark.getBoundingClientRect();
      mark.dispatchEvent(
        new PointerEvent("pointermove", {
          pointerId: 1,
          bubbles: true,
          clientX: box.left + box.width / 2,
          clientY: box.top + box.height / 2,
        }),
      );
      await tipShowsTheName();
      expect(name?.classList.contains("rolling")).toBe(false);
    } finally {
      reduce.mockRestore();
    }
  });
});
