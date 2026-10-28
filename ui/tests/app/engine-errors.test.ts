// Every error the engine can report says on screen what went wrong, in words of its own: a code
// without them would fall back to the bare "Display error".
import { beforeAll, describe, expect, test } from "vitest";

import { ErrorCode } from "../../../protocol/src/messages.ts";
import { byId, startApp, type Started } from "./harness.ts";

describe("engine errors", () => {
  let app: Started;
  beforeAll(async () => {
    app = await startApp();
  });

  test.each(Object.values(ErrorCode))(
    "%s explains itself over the screen",
    async (code) => {
      const detail = "Engine detail.";
      app.deliver({ type: "error", code, message: detail });
      await expect.element(byId("message")).toBeVisible();
      expect(byId("message-title").textContent).not.toBe("Display error");
      expect(byId("message-text").textContent).not.toBe("");
      if (code === ErrorCode.protocolMismatch) {
        // The engine names both versions; the page adds what to do.
        expect(byId("message-text").textContent).toContain(detail);
      }
    },
  );
});
