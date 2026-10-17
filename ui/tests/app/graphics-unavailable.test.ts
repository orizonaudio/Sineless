// A web view that gives the page no WebGL2, as on a machine whose graphics driver is blocklisted:
// the page says why the traces are missing instead of staying blank, and the rest still works.
import { beforeAll, describe, expect, test, vi } from "vitest";
import { userEvent } from "vitest/browser";

import { byId, choice, settle, startApp } from "./harness.ts";

describe("a web view without WebGL2", () => {
  beforeAll(async () => {
    type GetContext = HTMLCanvasElement["getContext"];
    // The browser's own, for the grid's 2D context.
    const { value: getContext } = Object.getOwnPropertyDescriptor(
      HTMLCanvasElement.prototype,
      "getContext",
    ) as TypedPropertyDescriptor<GetContext>;
    vi.spyOn(HTMLCanvasElement.prototype, "getContext").mockImplementation(
      function (this: HTMLCanvasElement, ...args: Parameters<GetContext>) {
        return args[0] === "webgl2"
          ? null
          : Reflect.apply(getContext as GetContext, this, args);
      } as GetContext,
    );
    await startApp();
  });

  test("shows a message saying the graphics are unavailable", () => {
    expect(byId("message").hidden).toBe(false);
    expect(byId("message-title").textContent).not.toBe("");
    expect(byId("message-text").textContent).not.toBe("");
  });

  test("keeps the message once the engine's state arrives", async () => {
    await settle();
    expect(byId("message").hidden).toBe(false);
  });

  test("still answers its controls", async () => {
    await userEvent.click(choice("display-mode", "Multi"));
    await settle();
    expect(choice("display-mode", "Multi").getAttribute("aria-pressed")).toBe(
      "true",
    );
  });
});
