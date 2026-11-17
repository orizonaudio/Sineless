// The Layout popover's Traces row (#432), through the real page and the mock back end: Line draws each
// trace's wave as a line, Filled the area between it and zero, in Single and Multi, stacked and
// overlaid; the choice is the engine's, so a host's change shows at once and the page reopened
// keeps it.
import { describe, expect, test, vi } from "vitest";

import { TraceShape } from "../../../protocol/src/enums.ts";
import type { Frame } from "../../../protocol/src/frame-decoder.ts";
import {
  TraceRenderer,
  type TraceView,
} from "../../src/render/trace-renderer.ts";
import {
  byId,
  chips,
  choice,
  press,
  settle,
  startApp,
  type Started,
} from "./harness.ts";

let drawnShape: TraceShape | undefined;
let watching = false;

// The style the page's renderer draws its next frame in: what it is given at each draw, whose
// pictures in each style the drawing tests check (trace-drawing.test.ts, a filled trace).
function watchDraws(): void {
  if (watching) {
    return;
  }
  watching = true;
  const draw = Object.getOwnPropertyDescriptor(TraceRenderer.prototype, "draw")
    ?.value as (this: TraceRenderer, frame: Frame) => void;
  vi.spyOn(TraceRenderer.prototype, "draw").mockImplementation(function (
    this: TraceRenderer,
    frame: Frame,
  ) {
    draw.call(this, frame);
    drawnShape = (this as unknown as { view: () => TraceView }).view()
      .traceShape;
  });
}

async function nextShape(): Promise<TraceShape | undefined> {
  watchDraws();
  drawnShape = undefined;
  await vi.waitFor(
    () => {
      expect(drawnShape).toBeDefined();
    },
    { timeout: 5000 },
  );
  return drawnShape;
}

function tracesRow(label: "Line" | "Filled"): HTMLButtonElement {
  return choice("trace-shape-choice", label);
}

async function setTraces(label: "Line" | "Filled"): Promise<void> {
  const button = tracesRow(label);
  await press(button);
  await expect.element(button).toHaveAttribute("aria-pressed", "true");
}

describe("the Traces row", () => {
  test("is in the Layout popover under Traces, Line in a new instance, and asks the engine for the choice", async () => {
    const app = await startApp();
    const popover = byId("layout-popover");
    expect(popover.contains(tracesRow("Line"))).toBe(true);
    const rows = [...popover.querySelectorAll(".popover-row")];
    expect(
      rows.indexOf(tracesRow("Line").closest(".popover-row") as Element),
    ).toBe(
      rows.indexOf(
        choice("layout", "Stack").closest(".popover-row") as Element,
      ) + 1,
    );
    expect(tracesRow("Line").getAttribute("aria-pressed")).toBe("true");
    await press(tracesRow("Filled"));
    const sent = app.sent.filter((message) => message.type === "setSetting");
    expect(sent.at(-1)).toEqual({
      type: "setSetting",
      name: "traceShape",
      value: "filled",
    });
  });

  // Each frame is drawn in the style chosen, in every mode and layout.
  test.each(
    (["Single", "Multi"] as const).flatMap((mode) =>
      (["Stack", "Overlay"] as const).map((layout) => ({ mode, layout })),
    ),
  )("draws the traces filled in $mode, $layout", async ({ mode, layout }) => {
    await startApp();
    await press(choice("time-base", "Trig"));
    await press(choice("display-mode", mode));
    await press(choice("layout", layout));
    expect(tracesRow("Filled").disabled).toBe(false);
    await settle();
    await expect.poll(nextShape).toBe(TraceShape.line);
    await setTraces("Filled");
    await expect.poll(nextShape).toBe(TraceShape.filled);
    await setTraces("Line");
    await expect.poll(nextShape).toBe(TraceShape.line);
  });

  // Chosen by the host (automation, an undo), it shows at once.
  test("follows the host's choice: the lit button and the drawing", async () => {
    const app = await startApp();
    await press(choice("time-base", "Trig"));
    await settle();
    await expect.poll(nextShape).toBe(TraceShape.line);
    app.transport.send({
      type: "setSetting",
      name: "traceShape",
      value: "filled",
    });
    await expect
      .element(tracesRow("Filled"))
      .toHaveAttribute("aria-pressed", "true");
    await expect.poll(nextShape).toBe(TraceShape.filled);
  });

  // While Normal waits for a trigger no frame comes, maybe for good: the choice redraws the
  // traces at once, so the screen never shows a style other than the one lit.
  test("redraws the traces in the style chosen while no frame comes", async () => {
    const app = await startApp();
    await press(choice("time-base", "Trig"));
    await press(choice("mode", "Normal"));
    await settle();
    await expect.poll(nextShape).toBe(TraceShape.line);
    app.holdFrames(true);
    try {
      await expect.poll(chips, { timeout: 3000 }).toContain("Waiting");
      await setTraces("Filled");
      await expect.poll(nextShape).toBe(TraceShape.filled);
      await setTraces("Line");
      await expect.poll(nextShape).toBe(TraceShape.line);
    } finally {
      app.holdFrames(false);
    }
  });

  test("is the engine's: the page reopened shows Filled", async () => {
    const first: Started = await startApp();
    await setTraces("Filled");
    await startApp({}, first.transport);
    await settle();
    expect(tracesRow("Filled").getAttribute("aria-pressed")).toBe("true");
  });
});
