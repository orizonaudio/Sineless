import { expect, test } from "vitest";

import { protocolVersion } from "../../protocol/src/version";

test("ui resolves the protocol module", () => {
  expect(protocolVersion).toBe(1);
});
