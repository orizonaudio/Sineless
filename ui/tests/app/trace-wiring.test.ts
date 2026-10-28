// The traces through the real page: every lane of each view is drawn, a track left out of the
// comparison is fainter than a compared one, and a trace waiting for a trigger is fainter than
// the same trace drawn live. The pixels are read straight after each draw.
import { beforeAll, describe, expect, test, vi } from "vitest";
import { userEvent } from "vitest/browser";

import { ViewLayout } from "../../../protocol/src/enums.ts";
import type { Frame } from "../../../protocol/src/frame-decoder.ts";
import { lanes } from "../../src/render/trace-geometry.ts";
import { TraceRenderer } from "../../src/render/trace-renderer.ts";
import {
  byId,
  chips,
  choice,
  press,
  settle,
  startApp,
  type Started,
} from "./harness.ts";

// The most opaque pixel of each lane of a frame drawn (one lane for overlaid views), measured
// on request only: reading the pixels back at every frame would slow the page.
interface Measured {
  readonly frame: Frame;
  readonly laneAlpha: readonly number[];
}
let wanted = false;
// Every draw measured: while frames are held, the page draws only when its state changes.
let everyDraw = false;
let drawn: Measured | undefined;
// Multi-track views are shown stacked in these tests.
const layout = ViewLayout.stacked;

// The next frame the page draws, measured.
async function nextDraw(): Promise<Measured> {
  drawn = undefined;
  wanted = true;
  await vi.waitFor(
    () => {
      expect(drawn).toBeDefined();
    },
    { timeout: 5000 },
  );
  return drawn as unknown as Measured;
}

function laneNames(): string[] {
  return [...byId("lane-labels").querySelectorAll(".lane-label")].map(
    (label) => label.textContent,
  );
}

function compareCheck(name: string): HTMLButtonElement {
  const check = [...byId("lane-labels").querySelectorAll("button")].find(
    (button) =>
      button.classList.contains("compare-check") && button.textContent === name,
  );
  expect(check, `check ${name}`).toBeDefined();
  return check as HTMLButtonElement;
}

let app: Started;

beforeAll(async () => {
  // The renderer's own draw, called from the spy that replaces it.
  const draw = Object.getOwnPropertyDescriptor(TraceRenderer.prototype, "draw")
    ?.value as (this: TraceRenderer, frame: Frame) => void;
  vi.spyOn(TraceRenderer.prototype, "draw").mockImplementation(function (
    this: TraceRenderer,
    frame: Frame,
  ) {
    draw.call(this, frame);
    if (!wanted && !everyDraw) {
      return;
    }
    wanted = false;
    const canvas = byId("trace") as HTMLCanvasElement;
    const gl = canvas.getContext("webgl2");
    if (gl === null) {
      return;
    }
    const { width, height } = canvas;
    const pixels = new Uint8Array(width * height * 4);
    gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    const count = Math.max(frame.lanes.length, 1);
    const laneAlpha = lanes(
      count,
      frame.lanes.length > 0 ? layout : ViewLayout.overlaid,
    ).map(({ center, halfHeight }) => {
      const bottom = Math.ceil(((center - halfHeight + 1) / 2) * height);
      const top = Math.floor(((center + halfHeight + 1) / 2) * height);
      let most = 0;
      for (let y = bottom; y < top; y++) {
        for (let x = 0; x < width; x++) {
          most = Math.max(most, pixels[(y * width + x) * 4 + 3] ?? 0);
        }
      }
      return most / 255;
    });
    drawn = { frame, laneAlpha };
  });
  app = await startApp();
});

describe("every lane is drawn", () => {
  test("in the Single view", async () => {
    await userEvent.click(choice("display-mode", "Single"));
    await settle();
    expect((await nextDraw()).laneAlpha[0]).toBeGreaterThan(0.5);
  });

  test.each(["Multi", "Compare"] as const)(
    "in the %s view, stacked",
    async (mode) => {
      await userEvent.click(choice("time-base", "Sec"));
      await userEvent.click(choice("display-mode", mode));
      await press(choice("group", "A"));
      await userEvent.click(choice("layout", "Stack"));
      await settle();
      const { frame, laneAlpha } = await nextDraw();
      expect(frame.lanes.length).toBeGreaterThan(1);
      expect(laneAlpha).toHaveLength(frame.lanes.length);
      laneAlpha.forEach((alpha, lane) => {
        expect(alpha, `lane ${String(lane)}`).toBeGreaterThan(0);
      });
    },
  );
});

test("a track left out of the comparison is fainter than a compared one", async () => {
  await userEvent.click(choice("display-mode", "Compare"));
  await press(choice("group", "A"));
  await userEvent.click(choice("layout", "Stack"));
  if (compareCheck("Kick 2").getAttribute("aria-checked") === "true") {
    await userEvent.click(compareCheck("Kick 2"));
  }
  await expect
    .poll(() => compareCheck("Kick 2").getAttribute("aria-checked"))
    .toBe("false");
  await settle();
  const names = laneNames();
  const { laneAlpha } = await nextDraw();
  expect(laneAlpha).toHaveLength(names.length);
  const left = laneAlpha[names.indexOf("Kick 2")] ?? Number.NaN;
  const compared = laneAlpha[names.indexOf("Kick")] ?? Number.NaN;
  expect(left).toBeGreaterThan(0);
  expect(left).toBeLessThan(compared);
});

test("a trace waiting for a trigger is fainter than the same trace live", async () => {
  await userEvent.click(choice("display-mode", "Single"));
  await press(choice("mode", "Normal"));
  await settle();
  const live = (await nextDraw()).laneAlpha[0] ?? Number.NaN;
  everyDraw = true;
  app.holdFrames(true);
  await expect.poll(chips, { timeout: 3000 }).toContain("Waiting");
  // The page redraws the last frame dimmed once it says it is waiting.
  await expect
    .poll(() => drawn?.laneAlpha[0] ?? Number.NaN, { timeout: 3000 })
    .toBeLessThan(live);
  expect(drawn?.laneAlpha[0]).toBeGreaterThan(0);
  everyDraw = false;
  app.holdFrames(false);
  await press(choice("mode", "Auto"));
});
