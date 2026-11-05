// The screen can change size with no window resize: at start-up the page's own layout settles after
// its first measurement, with the window already at its size. The page sizes its canvases, tells
// the engine its width and places what it draws in pixels from the screen itself, so the traces
// are drawn at the screen's size, never stretched, blurred or blank, whatever changed it.
import { afterEach, describe, expect, test } from "vitest";

import { MockTransport } from "../../src/bridge/mock-transport.ts";
import type { ControlMessage } from "../../src/bridge/transport.ts";
import { byId, settle, startApp, type Started } from "./harness.ts";

const screen = () => byId("screen");
const canvas = (id: string) => byId(id) as HTMLCanvasElement;

// The page as it sees the display: the pixel ratio a test runs at, restored after it.
const ratio = Object.getOwnPropertyDescriptor(window, "devicePixelRatio");
function atRatio(value: number): void {
  Object.defineProperty(window, "devicePixelRatio", {
    configurable: true,
    get: () => value,
  });
}
afterEach(() => {
  if (ratio !== undefined) {
    Object.defineProperty(window, "devicePixelRatio", ratio);
  }
});

// Both canvases at the screen's size in device pixels.
function expectCanvasesAtScreenSize(): void {
  for (const id of ["grid", "trace"]) {
    expect(canvas(id).width).toBe(
      Math.round(screen().clientWidth * window.devicePixelRatio),
    );
    expect(canvas(id).height).toBe(
      Math.round(screen().clientHeight * window.devicePixelRatio),
    );
  }
}

const viewports = (app: Started) =>
  app.sent.filter((message) => message.type === "viewport");

describe("the screen changing size with no window resize", () => {
  // Each dimension the page compares, and a change of one pixel, at a 1x and a Retina ratio.
  const changes = [
    { name: "the width alone", width: -120, height: 0 },
    { name: "the height alone", width: 0, height: -90 },
    { name: "both", width: -60, height: -40 },
    { name: "one pixel of width", width: -1, height: 0 },
  ];
  test.each(
    [1, 2].flatMap((pixelRatio) =>
      changes.map((change) => ({ ...change, pixelRatio })),
    ),
  )(
    "sizes the canvases to the screen when $name changes, at ratio $pixelRatio",
    async ({ width, height, pixelRatio }) => {
      atRatio(pixelRatio);
      const app = await startApp();
      await settle();
      const before = {
        width: screen().clientWidth,
        height: screen().clientHeight,
      };
      const sent = app.sent.length;
      // The screen's own style: no window resize event.
      screen().style.width = `${String(before.width + width)}px`;
      screen().style.height = `${String(before.height + height)}px`;
      await settle();
      expect(screen().clientWidth).toBe(before.width + width);
      expect(screen().clientHeight).toBe(before.height + height);
      expectCanvasesAtScreenSize();
      if (width !== 0) {
        expect(
          app.sent
            .slice(sent)
            .filter((message) => message.type === "viewport")
            .at(-1),
        ).toMatchObject({ viewWidth: before.width + width, pixelRatio });
      }
    },
  );

  // The measured fault: the page's first measurement taken before its layout settled. Here the
  // screen changes as the page says hello, after the measurement, before any rendering update.
  test("follows a screen that settles after the page first measured it", async () => {
    let settledWidth = 0;
    const transport = new (class extends MockTransport {
      override send(message: ControlMessage): void {
        if (message.type === "hello") {
          settledWidth = byId("screen").clientWidth - 80;
          byId("screen").style.width = `${String(settledWidth)}px`;
        }
        super.send(message);
      }
    })();
    const app = await startApp({}, transport);
    await settle();
    expectCanvasesAtScreenSize();
    expect(viewports(app).at(-1)).toMatchObject({ viewWidth: settledWidth });
  });

  // Resizing a canvas clears it, and the screen's change is seen after the frame's draw: the
  // traces are drawn again before that frame is shown, so it is never blank.
  test("shows the traces in the frame the screen changed size", async () => {
    await startApp();
    await settle();
    const before = screen().clientWidth;
    // What the observer called after the app's saw, at each call.
    const seen: { width: number; coverage: number }[] = [];
    // Made after the app's: called after it in the same rendering update.
    const observer = new ResizeObserver(() => {
      const trace = canvas("trace");
      const gl = trace.getContext("webgl2");
      if (gl === null || trace.width === 0) {
        return;
      }
      const pixels = new Uint8Array(trace.width * trace.height * 4);
      gl.readPixels(
        0,
        0,
        trace.width,
        trace.height,
        gl.RGBA,
        gl.UNSIGNED_BYTE,
        pixels,
      );
      let coverage = 0;
      for (let i = 3; i < pixels.length; i += 4) {
        coverage += pixels[i] ?? 0;
      }
      seen.push({ width: trace.width, coverage });
    });
    observer.observe(screen());
    await settle();
    seen.length = 0;
    screen().style.width = `${String(before - 100)}px`;
    await settle();
    observer.disconnect();
    expect(seen.at(-1)?.width).toBe(
      Math.round((before - 100) * window.devicePixelRatio),
    );
    expect(seen.at(-1)?.coverage ?? 0).toBeGreaterThan(0);
  });

  // A window resize sizes the canvases before the screen's change is seen in the same rendering
  // update; the observation then finds nothing to do: the engine is told once, the canvases
  // cleared once.
  test("tells the engine once when the window and the screen change together", async () => {
    const app = await startApp();
    await settle();
    const sent = app.sent.length;
    screen().style.width = `${String(screen().clientWidth - 70)}px`;
    window.dispatchEvent(new Event("resize"));
    await settle();
    expect(
      app.sent.slice(sent).filter((message) => message.type === "viewport"),
    ).toHaveLength(1);
  });

  // Level cursors are placed in pixels from the screen's height: a shorter screen moves each line
  // to the same level on it.
  test("keeps the level cursors at their levels", async () => {
    await startApp();
    await settle();
    byId("level-cursors").click();
    await settle();
    const line = () =>
      byId("level-cursor-lines").querySelector<HTMLElement>(".level-cursor");
    const share = () =>
      Number.parseFloat(line()?.style.top ?? "NaN") / screen().clientHeight;
    const before = share();
    expect(before).toBeGreaterThan(0);
    screen().style.height = `${String(screen().clientHeight - 120)}px`;
    await settle();
    expect(share()).toBeCloseTo(before, 6);
  });
});
