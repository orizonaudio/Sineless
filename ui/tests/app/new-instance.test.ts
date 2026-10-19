// A new instance, opened with the settings the engine starts with (the protocol's defaults).
import { beforeAll, describe, expect, test } from "vitest";

import { byId, choice, settle, startApp } from "./harness.ts";

describe("a new instance", () => {
  beforeAll(async () => {
    await startApp();
    await settle();
  });

  test("shows each channel view in a lane of its own", async () => {
    // Each lane has its own zero line, labelled −∞: L's in the screen's top half and R's in its
    // bottom half. Overlaid, the views would share one, in the middle.
    const screen = byId("screen").getBoundingClientRect();
    const zeroLines = [...byId("labels").querySelectorAll(".axis-label")]
      .filter((label) => label.textContent === "−∞")
      .map(
        (label) =>
          (label.getBoundingClientRect().top - screen.top) / screen.height,
      );
    expect(zeroLines).toHaveLength(2);
    expect(zeroLines.filter((position) => position < 0.5)).toHaveLength(1);
    await expect
      .element(choice("layout", "Stack"))
      .toHaveAttribute("aria-pressed", "true");
  });
});
