// The graphics context lost during a session, as when the GPU is reset or its driver updated:
// the page says the traces stopped instead of freezing them silently, and draws them again once
// the context comes back.
import { beforeAll, describe, expect, test, vi } from "vitest";

import { byId, settle, startApp, type Started } from "./harness.ts";

const context = () =>
  (byId("trace") as HTMLCanvasElement).getContext(
    "webgl2",
  ) as WebGL2RenderingContext;

describe("a lost graphics context", () => {
  let app: Started;
  // Taken before the loss: while the context is lost, it gives out no extensions.
  let extension: WEBGL_lose_context;

  beforeAll(async () => {
    app = await startApp();
    const found = context().getExtension("WEBGL_lose_context");
    expect(found, "WEBGL_lose_context").not.toBeNull();
    extension = found as WEBGL_lose_context;
  });

  test("shows a message while the traces cannot be drawn", async () => {
    expect(byId("message").hidden).toBe(true);
    extension.loseContext();
    await expect.poll(() => byId("message").hidden).toBe(false);
    expect(byId("message-title").textContent).not.toBe("");
  });

  // With no frame coming, as while frozen or while nothing triggers, the last one is drawn again:
  // the restored canvas starts empty.
  test("draws the traces again once the context comes back, without a new frame", async () => {
    await settle();
    app.holdFrames(true);
    const frames = () => window.orizonDebug?.frameCount ?? 0;
    // Until the frame already asked for has come.
    await new Promise((resolve) => setTimeout(resolve, 500));
    const held = frames();
    const gl = context();
    const clear = vi.spyOn(WebGL2RenderingContext.prototype, "clear");
    try {
      extension.restoreContext();
      await expect.poll(() => byId("message").hidden).toBe(true);
      await expect.poll(() => clear.mock.calls.length).toBeGreaterThan(0);
      expect(frames(), "frames received").toBe(held);
      // Drawing with the programs and buffers of the lost context fails.
      expect(gl.getError()).toBe(gl.NO_ERROR);
    } finally {
      clear.mockRestore();
      app.holdFrames(false);
    }
  });

  test("keeps the message when the traces cannot be drawn on the restored context", async () => {
    extension.loseContext();
    await expect.poll(() => byId("message").hidden).toBe(false);
    const gl = context();
    const getContext = vi
      .spyOn(HTMLCanvasElement.prototype, "getContext")
      .mockReturnValue(null);
    try {
      extension.restoreContext();
      await expect.poll(() => gl.isContextLost()).toBe(false);
      await settle();
      expect(byId("message").hidden).toBe(false);
    } finally {
      getContext.mockRestore();
    }
  });
});
