// A recorded session fails a test that ends on a view the page sent short of the recorded one:
// the page would otherwise be tested on the frame before it.
import { beforeAll, describe, test } from "vitest";

import { byId, settle, startRecorded } from "./harness.ts";

describe("a page that stops zooming short of the recorded view", () => {
  beforeAll(async () => {
    await startRecorded("Roll1500msDeepestZoom_48k");
  });

  // The check after the test reports the view it stopped at.
  test.fails("fails the test it stops in", async () => {
    await settle();
    const trace = byId("trace");
    const bounds = trace.getBoundingClientRect();
    trace.dispatchEvent(
      new WheelEvent("wheel", {
        deltaY: -400,
        metaKey: true,
        clientX: bounds.left + bounds.width / 2,
        clientY: bounds.top + bounds.height / 2,
        bubbles: true,
        cancelable: true,
      }),
    );
    await settle(1);
  });
});
