// The graphics context lost and restored while the engine reports an error that nothing clears,
// as when the editor has no engine or the page and the plugin come from different releases: the
// error, which says why the screen is empty and what to do, stays on screen through both.
import { beforeAll, describe, expect, test } from "vitest";

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

  test("an error shown before the context is lost keeps its text through the loss and the restore", async () => {
    expect(shown()).toBe("Display unavailable");
    extension.loseContext();
    await expect.poll(() => context().isContextLost()).toBe(true);
    await new Promise((resolve) => setTimeout(resolve, 300));
    expect(shown()).toBe("Display unavailable");
    extension.restoreContext();
    await expect.poll(() => context().isContextLost()).toBe(false);
    await new Promise((resolve) => setTimeout(resolve, 300));
    expect(shown()).toBe("Display unavailable");
  });
});
