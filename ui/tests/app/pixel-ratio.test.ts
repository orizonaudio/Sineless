// Moving the window to a display of another pixel ratio (a Retina screen and a 1x monitor)
// changes no size, so no resize event arrives: the page watches the ratio itself.
import {
  beforeAll,
  describe,
  expect,
  test,
  vi,
  type MockInstance,
} from "vitest";

import { byId, settle, startApp, type Started } from "./harness.ts";

describe("on a display of another pixel ratio", () => {
  let app: Started;
  const after = window.devicePixelRatio * 2;
  // The browser's signal, as a MediaQueryList gives it: a "change" when a resolution query starts
  // or stops matching the ratio, to each listener still on it (a listener added once comes off
  // after its first).
  interface Watcher {
    readonly resolution: number;
    readonly once: boolean;
    readonly changed: () => void;
  }
  const watching: Watcher[] = [];
  function moveTo(ratio: number): void {
    const before = window.devicePixelRatio;
    Object.defineProperty(window, "devicePixelRatio", {
      configurable: true,
      get: () => ratio,
    });
    const told = watching.filter(
      ({ resolution }) => (resolution === before) !== (resolution === ratio),
    );
    for (const watcher of told) {
      if (watcher.once) {
        watching.splice(watching.indexOf(watcher), 1);
      }
      watcher.changed();
    }
  }
  // The trace's line width as a share of the screen's height, at each draw, before and after
  // the move: the default view draws min/max columns at least this wide.
  let widths: number[] = [];
  let locations: MockInstance<WebGL2RenderingContext["getUniformLocation"]>;
  let uniforms: MockInstance<WebGL2RenderingContext["uniform1f"]>;

  function drawnWidths(): number[] {
    const minimumHalf = new Set<WebGLUniformLocation>();
    locations.mock.calls.forEach(([, name], index) => {
      const result = locations.mock.results[index];
      if (
        name === "minimumHalf" &&
        result?.type === "return" &&
        result.value !== null
      ) {
        minimumHalf.add(result.value);
      }
    });
    return uniforms.mock.calls
      .filter(([location]) => location !== null && minimumHalf.has(location))
      .map(([, value]) => value);
  }

  beforeAll(async () => {
    const matchMedia = window.matchMedia.bind(window);
    vi.spyOn(window, "matchMedia").mockImplementation((query) => {
      const list = matchMedia(query);
      if (query.includes("resolution")) {
        const resolution = Number(/resolution: ([\d.]+)dppx/.exec(query)?.[1]);
        vi.spyOn(list, "addEventListener").mockImplementation(
          (type, listener, options) => {
            if (type !== "change") {
              return;
            }
            watching.push({
              resolution,
              once: typeof options === "object" && options.once === true,
              changed: () => {
                if (typeof listener === "function") {
                  listener(new Event("change"));
                } else {
                  listener.handleEvent(new Event("change"));
                }
              },
            });
          },
        );
      }
      return list;
    });
    locations = vi.spyOn(
      WebGL2RenderingContext.prototype,
      "getUniformLocation",
    );
    uniforms = vi.spyOn(WebGL2RenderingContext.prototype, "uniform1f");
    app = await startApp();
    await settle();
    moveTo(after);
    await settle();
    widths = drawnWidths();
  });

  test("draws at the new display's resolution", () => {
    const screen = byId("screen");
    for (const id of ["grid", "trace"]) {
      expect((byId(id) as HTMLCanvasElement).width).toBe(
        Math.round(screen.clientWidth * after),
      );
    }
    expect(app.sent.at(-1)).toMatchObject({
      type: "viewport",
      pixelRatio: after,
    });
  });

  test("draws traces as thick as on the display before", () => {
    const [first = Number.NaN] = widths;
    expect((widths.at(-1) ?? 0) / first).toBeCloseTo(1);
  });

  // A move resizes once, however many moves came before: a listener left on an old query would
  // be told again at each move back to its ratio, and the work would grow with every move.
  test("resizes once a move, moving back and forth between displays", async () => {
    for (const ratio of [1.25, after, 1.25, after, 1.25, after]) {
      const before = app.sent.length;
      moveTo(ratio);
      await settle();
      expect(
        app.sent.slice(before).filter((message) => message.type === "viewport"),
      ).toEqual([expect.objectContaining({ pixelRatio: ratio })]);
    }
  });

  // Last: it moves the window on to a third display.
  test("follows the window on to another display", async () => {
    moveTo(1.25);
    await settle();
    const screen = byId("screen");
    for (const id of ["grid", "trace"]) {
      expect((byId(id) as HTMLCanvasElement).width).toBe(
        Math.round(screen.clientWidth * 1.25),
      );
    }
    expect(app.sent.at(-1)).toMatchObject({
      type: "viewport",
      pixelRatio: 1.25,
    });
  });
});
