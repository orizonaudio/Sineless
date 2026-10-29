// A recorded session fails the test as soon as the page sends what its recording does not hold:
// a page change that makes a recording unreachable is never tested on stale frames.
import { beforeAll, describe, expect, test } from "vitest";
import { userEvent } from "vitest/browser";

import { byId, checkReplay, settle, startRecorded } from "./harness.ts";

describe("a page on a recorded session", () => {
  beforeAll(async () => {
    await startRecorded("SineHalf1k_44k1");
  });

  test("fails the step that sends a message the recording does not hold, naming it", async () => {
    await settle();
    // The recording holds no freeze.
    await userEvent.click(byId("freeze"));
    await expect(settle()).rejects.toThrow(
      /SineHalf1k_44k1: the page sent \{"type":"freeze","frozen":true\}, which the recording does not hold; it expected nothing more/u,
    );
    // Reported once: the failure is not repeated by the next step.
    expect(() => {
      checkReplay();
    }).not.toThrow();
  });

  // The check after each test catches a message no later step looked for.
  test.fails(
    "fails a test that ends with a message the recording does not hold",
    async () => {
      await userEvent.click(byId("freeze"));
      await expect
        .poll(() => byId("freeze").getAttribute("aria-pressed"))
        .toBe("false");
    },
  );
});
