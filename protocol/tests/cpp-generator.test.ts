import { describe, expect, test } from "vitest";

import { generateCppHeader } from "../src/cpp-generator.ts";
import { beatSpanLengths } from "../src/enums.ts";
import { frameHeader, layoutFields } from "../src/frame.ts";

describe("C++ generator", () => {
  const header = generateCppHeader();

  test("emits enums with kPascalCase enumerators and stable values", () => {
    expect(header).toContain("enum class TriggerMode : std::uint8_t");
    expect(header).toContain("    kSingle = 2,");
    expect(header).toContain(
      "inline constexpr std::size_t kChannelViewCount = 4;",
    );
  });

  test("emits one descriptor per setting with its range and default", () => {
    expect(header).toContain(
      '{SettingId::kTriggerSpan, "triggerSpan", SettingKind::kNumber, 0.0005, 1, 1, true, {}},',
    );
    expect(header).toContain(
      '{SettingId::kChannelViews, "channelViews", SettingKind::kFlags, 1, 15, 3, false, kChannelViewNames},',
    );
    expect(header).toContain(
      '{SettingId::kTriggerMode, "triggerMode", SettingKind::kEnum, 0, 2, 0, false, kTriggerModeNames},',
    );
  });

  test("emits enum value names in value order", () => {
    expect(header).toContain(
      'kTriggerModeNames = {\n    "auto",\n    "normal",\n    "single",\n};',
    );
  });

  // Each span's length as the table gives it, in table order, so the engine sweeps what the UI names.
  test("emits the beat span lengths", () => {
    const entries = beatSpanLengths
      .map((length) => `    {${String(length.amount)}},`)
      .join("\n");
    expect(header).toContain(`kBeatSpanLengths = {{\n${entries}\n}};`);
  });

  test("emits message type names", () => {
    expect(header).toContain('"setSetting",');
  });

  test("emits frame header offsets", () => {
    expect(header).toContain(
      `static constexpr std::size_t kSize = ${String(frameHeader.size)};`,
    );
    expect(header).toContain(
      "static constexpr std::size_t kStartFractionOffset = 8;",
    );
    expect(header).toContain("inline constexpr std::size_t kMatchValues = 3;");
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
