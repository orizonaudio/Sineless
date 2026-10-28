// An instance the host has not played through has no tempo yet. In Beats, the page draws no bars
// or times from a made-up tempo: it says it has none, and places them once the host reports one.
import { beforeAll, describe, expect, test } from "vitest";

import { DisplayMode } from "../../../protocol/src/enums.ts";
import {
  byId,
  choice,
  chips,
  press,
  settle,
  startApp,
  type Started,
} from "./harness.ts";

// The time axis's labels (bar positions in Beats, times in Seconds): those along the screen, not
// the level labels down its left edge.
function timeAxisLabels(): string[] {
  return [...byId("labels").querySelectorAll<HTMLElement>(".axis-label")]
    .filter((label) => label.style.left !== "13px")
    .map((label) => label.firstChild?.textContent ?? "");
}

describe("before the host reports a tempo", () => {
  let app: Started;
  beforeAll(async () => {
    app = await startApp({ hostTempo: false });
  });

  test("says so, and places no bars", async () => {
    await expect.poll(chips).toEqual(["No tempo"]);
    await settle();
    expect(timeAxisLabels()).toEqual([]);
  });

  test("places the bars once the host reports its tempo", async () => {
    app.transport.reportTempo();
    await expect.poll(chips).toContain("Sync");
    await expect.poll(() => timeAxisLabels().length).toBeGreaterThan(0);
    expect(chips()).not.toContain("No tempo");
  });
});

// A frame with no tempo carries no lanes, even in Multi and Compare: the readout tag still names
// what those modes measure, this track, and in Single the readout channel.
describe("the readout tag before the host reports a tempo", () => {
  beforeAll(async () => {
    await startApp({ hostTempo: false });
  });

  test.each(
    Object.keys(DisplayMode).map((key) => [
      key.charAt(0).toUpperCase() + key.slice(1),
      key === "single" ? "Mid" : "This track",
    ]),
  )("in %s says %s", async (mode, tag) => {
    await press(choice("display-mode", mode));
    await expect.poll(chips).toEqual(["No tempo"]);
    await settle();
    expect(byId("readout-tag").textContent).toBe(tag);
  });
});
