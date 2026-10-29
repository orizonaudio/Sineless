// The graphics memory the traces are drawn through refused, at start or when the window grows:
// the page says the graphics are unavailable instead of drawing nothing. A screen hidden to no
// size at all is not a failure.
import { afterEach, describe, expect, test, vi } from "vitest";

import { graphicsMessages } from "../../src/app.ts";
import { byId, settle, startApp } from "./harness.ts";

const shown = () =>
  byId("message").hidden ? undefined : byId("message-title").textContent;
// The message says the memory was refused, not that WebGL2 is missing, and that audio passes.
const saysMemoryRefused = () => {
  expect(byId("message-text").textContent).toBe(graphicsMessages.memory);
  expect(graphicsMessages.memory).not.toBe(graphicsMessages.noWebGL2);
};

// The framebuffer the traces' coverage is drawn into reported incomplete from now on.
function refuseTraceMemory(): void {
  vi.spyOn(
    WebGL2RenderingContext.prototype,
    "checkFramebufferStatus",
  ).mockReturnValue(WebGL2RenderingContext.FRAMEBUFFER_INCOMPLETE_ATTACHMENT);
}

function resizeScreen(style: Partial<CSSStyleDeclaration>): void {
  Object.assign(byId("screen").style, style);
  window.dispatchEvent(new Event("resize"));
}

afterEach(() => {
  vi.restoreAllMocks();
});

describe("graphics memory for the traces", () => {
  test("refused at start, says the graphics are unavailable", async () => {
    refuseTraceMemory();
    await startApp();
    expect(shown()).toBe("Graphics unavailable");
    saysMemoryRefused();
  });

  test("refused when the window grows, says the graphics are unavailable", async () => {
    await startApp();
    await settle();
    expect(shown()).toBeUndefined();
    refuseTraceMemory();
    resizeScreen({ width: "1100px" });
    await expect.poll(shown).toBe("Graphics unavailable");
    saysMemoryRefused();
  });

  test("a screen hidden to no size, then shown again, still draws with no message", async () => {
    await startApp();
    await settle();
    resizeScreen({ display: "none" });
    expect((byId("trace") as HTMLCanvasElement).width).toBe(0);
    await settle();
    resizeScreen({ display: "" });
    await settle();
    expect(shown()).toBeUndefined();
  });
});
