// A Seconds span longer than the capture keeps (a host changed the sample rate while processing)
// shows what is kept: the time axis measures that, not the Span setting.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { formatSeconds } from "../../src/view/format.ts";
import { byId, choice, chips, settle, startApp, typeInto } from "./harness.ts";

const kept = 2;

function timeLabels(): string[] {
  return [...byId("labels").querySelectorAll(".axis-label")]
    .map((label) => label.firstChild?.textContent ?? "")
    .filter((text) => text.endsWith("s"));
}

describe("a span longer than the capture keeps", () => {
  beforeAll(async () => {
    await startApp({ keptSeconds: kept });
  });

  test("labels the time axis with the span shown", async () => {
    await userEvent.click(choice("time-base", "Sec"));
    await typeInto("span-field", "4 s");
    await expect.poll(chips).toContain("Span limited");
    await settle();
    // Rolling, the leftmost label is a fifth into the screen: four fifths of the span ago.
    await expect.poll(timeLabels).toContain(`−${formatSeconds(0.8 * kept)}`);
    expect(timeLabels()).not.toContain(`−${formatSeconds(0.8 * 4)}`);
  });
});
