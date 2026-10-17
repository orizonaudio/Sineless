// The graphics context lost and restored while the engine reports an error that nothing clears,
// as when the editor has no engine or the page and the plugin come from different releases: the
// error, which says why the screen is empty and what to do, stays on screen through both.
import { beforeAll, describe, expect, test, vi } from "vitest";

import { byId, startApp, type Started } from "./harness.ts";

const context = () =>
  (byId("trace") as HTMLCanvasElement).getContext(
    "webgl2",
  ) as WebGL2RenderingContext;
const shown = () =>
  byId("message").hidden ? undefined : byId("message-title").textContent;

describe("an engine error and the graphics context", () => {
  let app: Started;
  // Taken before any loss: while the context is lost, it gives out no extensions.
  let extension: WEBGL_lose_context;

  beforeAll(async () => {
    app = await startApp();
    const found = context().getExtension("WEBGL_lose_context");
    expect(found, "WEBGL_lose_context").not.toBeNull();
    extension = found as WEBGL_lose_context;
  });

  test("an error that comes while the context is lost stays once the context comes back", async () => {
    extension.loseContext();
    await expect.poll(shown).toBe("Graphics unavailable");
    app.deliver({ type: "error", code: "engineUnavailable", message: "" });
    await expect.poll(shown).toBe("Display unavailable");
    extension.restoreContext();
    await expect.poll(() => context().isContextLost()).toBe(false);
    await new Promise((resolve) => setTimeout(resolve, 300));
    expect(shown()).toBe("Display unavailable");
  });

  // Losing the context again: browsers stop restoring a page's context after a few losses, so
  // each test here loses it once.
  test("an error shown keeps its text through a loss, and a restore that cannot draw the traces", async () => {
    expect(shown()).toBe("Display unavailable");
    const lost = new Promise((resolve) => {
      byId("trace").addEventListener("webglcontextlost", resolve, {
        once: true,
      });
    });
    extension.loseContext();
    // A context is restored only once its loss has been dispatched (WEBGL_lose_context).
    await lost;
    await new Promise((resolve) => setTimeout(resolve, 300));
    expect(shown()).toBe("Display unavailable");
    const gl = context();
    const getContext = vi
      .spyOn(HTMLCanvasElement.prototype, "getContext")
      .mockReturnValue(null);
    try {
      extension.restoreContext();
      await expect.poll(() => gl.isContextLost()).toBe(false);
      await new Promise((resolve) => setTimeout(resolve, 300));
      expect(shown()).toBe("Display unavailable");
    } finally {
      getContext.mockRestore();
    }
  });
});
