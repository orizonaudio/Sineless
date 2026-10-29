// A frozen 1 s span panned back, on the real engine's frames: the page pans by the span shown and
// no further back than the frame says the freeze reaches.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { formatSeconds } from "../../src/view/format.ts";
import type { ReplayTransport } from "./replay-transport.ts";
import {
  byId,
  chips,
  settle,
  startRecorded,
  swipe,
  type Started,
} from "./harness.ts";

describe("a recorded frozen span panned back", () => {
  let app: Started<ReplayTransport>;
  beforeAll(async () => {
    app = await startRecorded("FrozenPan_48k");
  });

  test("pans back a quarter of the span for a quarter of the screen", async () => {
    await settle();
    await userEvent.click(byId("freeze"));
    await expect.poll(chips).toContain("Frozen");
    await settle();
    swipe(-0.25);
    await expect.poll(chips).toContain(`−${formatSeconds(0.25)}`);
  });

  test("pans as far as the freeze reaches, and no further", async () => {
    const reach = window.orizonDebug?.lastFrame?.panReach ?? 0;
    expect(reach).toBeGreaterThan(0.25);
    // To the reach: what is left of it, in screens of the 1 s span.
    swipe(-(reach - 0.25));
    await expect.poll(chips).toContain(`−${formatSeconds(reach)}`);
    await settle();
    // Past it: held at the reach.
    swipe(-1);
    await settle();
    expect(chips()).toContain(`−${formatSeconds(reach)}`);
    // The page never asks for more than the freeze holds.
    const pans = app.transport.steps.filter((m) => m.type === "pan");
    expect(pans.length).toBeGreaterThan(0);
    for (const message of pans) {
      expect(message["seconds"]).toBeLessThanOrEqual(reach + 1e-6);
    }
  });
});
