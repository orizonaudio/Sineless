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
  // The browser's signal: the queries the page watches, by the resolution they ask for. Moving
  // to another ratio tells those that stop matching.
  const watching: { query: string; changed: () => void }[] = [];
  function moveTo(ratio: number): void {
    const before = `(resolution: ${String(window.devicePixelRatio)}dppx)`;
    Object.defineProperty(window, "devicePixelRatio", {
      configurable: true,
      get: () => ratio,
    });
    const stopped = watching.filter(({ query }) => query === before);
    watching.splice(
      0,
      watching.length,
      ...watching.filter(({ query }) => query !== before),
    );
    for (const { changed } of stopped) {
      changed();
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
        vi.spyOn(list, "addEventListener").mockImplementation(
          (_type, listener) => {
            watching.push({
              query,
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
