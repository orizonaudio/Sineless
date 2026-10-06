import { describe, expect, test } from "vitest";

import { generateCppHeader } from "../src/cpp-generator.ts";
import { frameHeader, layoutFields } from "../src/frame.ts";

describe("C++ generator", () => {
  const header = generateCppHeader();

  test("emits enums with kPascalCase enumerators and stable values", () => {
    expect(header).toContain("enum class TriggerMode : std::uint8_t");
    expect(header).toContain("    kSingle = 3,");
    expect(header).toContain(
      "inline constexpr std::size_t kChannelViewCount = 4;",
    );
  });

  test("emits one descriptor per setting with its range and default", () => {
    expect(header).toContain(
      '{SettingId::kTimeSpan, "timeSpan", SettingKind::kNumber, 0.0005, 4, 0.02, true},',
    );
    expect(header).toContain(
      '{SettingId::kChannelViews, "channelViews", SettingKind::kFlags, 1, 15, 3, false},',
    );
    expect(header).toContain(
      '{SettingId::kTriggerMode, "triggerMode", SettingKind::kEnum, 0, 3, 0, false},',
    );
  });

  test("emits message type names", () => {
    expect(header).toContain('"setSetting",');
  });

  test("emits frame header offsets", () => {
    expect(header).toContain(
      `static constexpr std::size_t kSize = ${String(frameHeader.size)};`,
    );
    expect(header).toContain(
      "static constexpr std::size_t kSamplesPerPointOffset = 12;",
    );
  });
});

describe("frame layout", () => {
  test("rejects a misaligned field", () => {
    expect(() =>
      layoutFields([
        { name: "a", type: "u8" },
        { name: "b", type: "u32" },
      ]),
    ).toThrow(/not aligned/);
  });
});
