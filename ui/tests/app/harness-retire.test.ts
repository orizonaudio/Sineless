// A test file that starts the app again (another track, or the editor reopened) leaves one app in
// the page: the one started before hears nothing more from its back end and asks it for nothing,
// and its timers stop, so it cannot write its own state into the new one's elements.
import { expect, test, vi } from "vitest";
import { userEvent } from "vitest/browser";

import { readoutIntervalSeconds } from "../../../protocol/src/limits.ts";
import { waitingAfterSeconds } from "../../src/view/status.ts";
import { chips, choice, press, settle, startApp } from "./harness.ts";

test("an app started earlier in the file asks its back end for nothing once another starts", async () => {
  const first = await startApp({ inputChannels: 1 });
  const requests = vi.spyOn(first.transport, "requestFrame");
  const sends = vi.spyOn(first.transport, "send");
  await startApp({ inputChannels: 2 });
  requests.mockClear();
  sends.mockClear();
  await settle(10);
  expect(requests).not.toHaveBeenCalled();
  expect(sends).not.toHaveBeenCalled();
});

// The earlier app, in Normal mode, gets no frames once retired: were its status timer still
// running, it would soon say "Waiting" in the new app's status bar, which shows Auto.
test("a new app's status bar shows no Waiting from an app started earlier in the file", async () => {
  await startApp();
  await userEvent.click(choice("time-base", "Sec"));
  await press(choice("mode", "Normal"));
  await settle();
  await startApp();
  await settle();
  await new Promise((resolve) => {
    setTimeout(resolve, 4000 * (waitingAfterSeconds + readoutIntervalSeconds));
  });
  expect(chips()).not.toContain("Waiting");
});
