// Moving the window to a display of another pixel ratio (a Retina screen and a 1x monitor)
// changes no size, so no resize event arrives: the page watches the ratio itself.
import { beforeAll, describe, expect, test, vi } from "vitest";

import { byId, settle, startApp, type Started } from "./harness.ts";

describe("on a display of another pixel ratio", () => {
  let app: Started;
  const after = window.devicePixelRatio * 2;
  // The browser's signal: a resolution query that stops matching.
  const watching: (() => void)[] = [];

  beforeAll(async () => {
    const matchMedia = window.matchMedia.bind(window);
    vi.spyOn(window, "matchMedia").mockImplementation((query) => {
      const list = matchMedia(query);
      if (query.includes("resolution")) {
        vi.spyOn(list, "addEventListener").mockImplementation(
          (_type, listener) => {
            watching.push(() => {
              if (typeof listener === "function") {
                listener(new Event("change"));
              } else {
                listener.handleEvent(new Event("change"));
              }
            });
          },
        );
      }
      return list;
    });
    app = await startApp();
    await settle();
    Object.defineProperty(window, "devicePixelRatio", {
      configurable: true,
      get: () => after,
    });
    for (const changed of watching.splice(0)) {
      changed();
    }
    await settle();
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

  test("keeps watching for the next move", () => {
    expect(watching.length).toBeGreaterThan(0);
  });
});
