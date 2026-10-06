import { expect, test } from "vitest";

import { protocolVersion } from "../src/version";

test("protocol version is a positive integer", () => {
  expect(Number.isInteger(protocolVersion)).toBe(true);
  expect(protocolVersion).toBeGreaterThan(0);
});
