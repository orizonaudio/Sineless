// An instance the host has not played through has no tempo yet. In Beats, the page draws no bars
// or times from a made-up tempo: it says it has none, and places them once the host reports one.
import { beforeAll, describe, expect, test } from "vitest";

import { byId, chips, settle, startApp, type Started } from "./harness.ts";

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
