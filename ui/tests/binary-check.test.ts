import { describe, expect, test } from "vitest";

import { decodeBase64, hasTestPattern } from "../src/spike/binary-check";

describe("binary check", () => {
  test("decodes base64 into bytes", () => {
    expect(Array.from(decodeBase64("AAEC/w=="))).toEqual([0, 1, 2, 255]);
  });

  test("accepts the test pattern across a wrap-around", () => {
    const bytes = Uint8Array.from({ length: 600 }, (_, i) => i % 256);
    expect(hasTestPattern(bytes)).toBe(true);
  });

  test("rejects a corrupted byte", () => {
    const bytes = Uint8Array.from({ length: 600 }, (_, i) => i % 256);
    bytes[300] = 0;
    expect(hasTestPattern(bytes)).toBe(false);
  });
});
