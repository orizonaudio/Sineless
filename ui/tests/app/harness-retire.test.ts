// A test file that starts the app again (another track, or the editor reopened) leaves one app in
// the page: the one started before hears nothing more from its back end and asks it for nothing,
// its timers stop and it watches its screen no more, so it cannot write its own state into the new
// one's elements.
import { afterEach, expect, test, vi } from "vitest";

import { readoutIntervalSeconds } from "../../../protocol/src/limits.ts";
import { waitingAfterSeconds } from "../../src/view/status.ts";
import { chips, choice, press, settle, startApp } from "./harness.ts";

test("an app started earlier in the file asks its back end for no frames once another starts", async () => {
  const first = await startApp({ inputChannels: 1 });
  const requests = vi.spyOn(first.transport, "requestFrame");
  await startApp({ inputChannels: 2 });
  requests.mockClear();
  await settle(10);
  expect(requests).not.toHaveBeenCalled();
});

// The earlier app, in Normal mode, gets no frames once retired: were its status timer still
// running, it would soon say "Waiting" in the new app's status bar, which shows Auto.
test("a new app's status bar shows no Waiting from an app started earlier in the file", async () => {
  await startApp();
  await press(choice("time-base", "Trig"));
  await press(choice("mode", "Normal"));
  await settle();
  await startApp();
  await settle();
  await new Promise((resolve) => {
    setTimeout(resolve, 4000 * (waitingAfterSeconds + readoutIntervalSeconds));
  });
  expect(chips()).not.toContain("Waiting");
});

// Replacing the page detaches the screen the earlier app observed, which browsers report as a
// change to no size: were it still observed, its sizing would draw its labels by id into the new
// app's page.
test("an app started earlier in the file watches its screen no more once another starts", async () => {
  const deliveries = new Map<ResizeObserver, number>();
  const native = window.ResizeObserver;
  vi.stubGlobal(
    "ResizeObserver",
    class extends native {
      constructor(callback: ResizeObserverCallback) {
        super((entries, observer) => {
          deliveries.set(observer, (deliveries.get(observer) ?? 0) + 1);
          callback(entries, observer);
        });
        deliveries.set(this, 0);
      }
    },
  );
  await startApp();
  await settle();
  const first = [...deliveries.keys()];
  expect(first.length).toBeGreaterThan(0);
  const before = first.map((observer) => deliveries.get(observer));
  await startApp();
  await settle();
  expect(first.map((observer) => deliveries.get(observer))).toEqual(before);
});

afterEach(() => {
  vi.unstubAllGlobals();
});
