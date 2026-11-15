// Multi's Channels list in the Views popover (#425): every Sineless instance in the host, those
// ticked shown as lanes. Through the real page on the mock engine: the list as the engine sends
// it, ticking, None and All, the most tracks Multi shows, instances that come and go, and the
// count in the Views field.
import { describe, expect, test } from "vitest";
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
    expect(count()).toBe("1/4 selected");
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
    expect(count()).toBe("3/4 selected");
    expect(multiSummary().startsWith("3/4·M·")).toBe(true);
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
    expect(count()).toBe("1/4 selected");
    await action("All");
    await expect.poll(tickedTracks).toHaveLength(4);
    await expect.poll(laneNames).toHaveLength(4);
    expect(count()).toBe("4/4 selected");
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
      "Views: 4 of 4 tracks, Mid, labels on",
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
    expect(count()).toBe(`${String(maxShownTracks)}/20 selected`);
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
    expect(count()).toBe("4/5 selected");
    expect(multiSummary().startsWith("4/5·")).toBe(true);
    await expect.poll(laneNames).toHaveLength(4);

    transport.removeTrack(2);
    // The unnamed track is the host's second now, in the list and on its lane.
    await expect.poll(names).toEqual(["Kick", "Track 2", "Kick 2", "Vox"]);
    await expect.poll(laneNames).toEqual(["Kick", "Track 2", "Kick 2"]);
    expect(count()).toBe("3/4 selected");
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
});
