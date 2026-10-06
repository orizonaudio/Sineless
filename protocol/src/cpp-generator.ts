// Generates the C++ header that mirrors this schema (DESIGN 5.1). Names follow DESIGN 8.3:
// enumerators and constants become kPascalCase.
import { enums, type EnumName } from "./enums.ts";
import { frameHeader, FrameFlag } from "./frame.ts";
import { MessageType } from "./messages.ts";
import { settings, type Setting } from "./settings.ts";
import { protocolVersion } from "./version.ts";

const pascalCase = (name: string): string =>
  name.charAt(0).toUpperCase() + name.slice(1);
const constant = (name: string): string => `k${pascalCase(name)}`;

function enumDeclaration(name: string, values: Record<string, number>): string {
  const entries = Object.entries(values)
    .map(([key, value]) => `    ${constant(key)} = ${String(value)},`)
    .join("\n");
  return [
    `enum class ${name} : std::uint8_t`,
    "{",
    entries,
    "};",
    `inline constexpr std::size_t ${constant(name)}Count = ${String(Object.keys(values).length)};`,
  ].join("\n");
}

// Names indexed by value, for readable JSON (values are 0..count-1 in declaration order).
function enumNamesDeclaration(
  name: string,
  values: Record<string, number>,
): string {
  const names = Object.entries(values)
    .sort(([, a], [, b]) => a - b)
    .map(([key]) => `    "${key}",`)
    .join("\n");
  return [
    `inline constexpr std::array<std::string_view, ${constant(name)}Count> ${constant(name)}Names = {`,
    names,
    "};",
  ].join("\n");
}

function enumValueCount(name: EnumName): number {
  return Object.keys(enums[name]).length;
}

function settingRange(setting: Setting): {
  minimum: number;
  maximum: number;
  logarithmic: boolean;
} {
  switch (setting.kind) {
    case "number":
      return setting;
    case "enum":
      return {
        minimum: 0,
        maximum: enumValueCount(setting.values) - 1,
        logarithmic: false,
      };
    case "flags":
      return {
        minimum: 1,
        maximum: (1 << enumValueCount(setting.values)) - 1,
        logarithmic: false,
      };
  }
}

function valueNames(setting: Setting): string {
  return setting.kind === "number" ? "{}" : `${constant(setting.values)}Names`;
}

function settingsDeclaration(): string {
  const entries = Object.entries(settings as Record<string, Setting>);
  const ids = entries.map(([name]) => `    ${constant(name)},`).join("\n");
  const descriptors = entries
    .map(([name, setting]) => {
      const range = settingRange(setting);
      const kind = constant(setting.kind);
      return (
        `    {SettingId::${constant(name)}, "${name}", SettingKind::${kind}, ` +
        `${String(range.minimum)}, ${String(range.maximum)}, ${String(setting.defaultValue)}, ` +
        `${String(range.logarithmic)}, ${valueNames(setting)}},`
      );
    })
    .join("\n");
  return [
    "enum class SettingId : std::uint8_t",
    "{",
    ids,
    "};",
    `inline constexpr std::size_t kSettingCount = ${String(entries.length)};`,
    "",
    "enum class SettingKind : std::uint8_t",
    "{",
    "    kNumber,",
    "    kEnum,",
    "    kFlags,",
    "};",
    "",
    "// Enum settings hold an index; flags settings hold a non-empty bit mask.",
    "// valueNames names the values of enum and flags settings and is empty for numbers.",
    "struct SettingDescriptor",
    "{",
    "    SettingId id;",
    "    std::string_view name;",
    "    SettingKind kind;",
    "    double minimum;",
    "    double maximum;",
    "    double defaultValue;",
    "    bool logarithmic;",
    "    std::span<const std::string_view> valueNames;",
    "};",
    "",
    "inline constexpr std::array<SettingDescriptor, kSettingCount> kSettings = {{",
    descriptors,
    "}};",
  ].join("\n");
}

function messageTypesDeclaration(): string {
  const names = Object.values(MessageType);
  return [
    enumDeclaration(
      "MessageType",
      Object.fromEntries(names.map((name, index) => [name, index])),
    ),
    `inline constexpr std::array<std::string_view, kMessageTypeCount> kMessageTypeNames = {`,
    names.map((name) => `    "${name}",`).join("\n"),
    "};",
  ].join("\n");
}

function frameDeclaration(): string {
  const offsets = frameHeader.fields
    .map(
      (field) =>
        `    static constexpr std::size_t ${constant(field.name)}Offset = ${String(field.offset)};`,
    )
    .join("\n");
  return [
    "// Byte offsets of the little-endian frame header; data follows at kSize.",
    "struct FrameHeader",
    "{",
    `    static constexpr std::size_t kSize = ${String(frameHeader.size)};`,
    offsets,
    "};",
    "",
    enumDeclaration("FrameFlag", FrameFlag),
  ].join("\n");
}

export function generateCppHeader(): string {
  const enumBlocks = Object.entries(enums).map(
    ([name, values]) =>
      `${enumDeclaration(name, values)}\n${enumNamesDeclaration(name, values)}`,
  );
  return [
    "// Generated from protocol/src by protocol/scripts/generate-cpp.ts. Do not edit.",
    "#pragma once",
    "",
    "#include <array>",
    "#include <cstddef>",
    "#include <cstdint>",
    "#include <span>",
    "#include <string_view>",
    "",
    "namespace orizon::protocol {",
    "",
    `inline constexpr int kProtocolVersion = ${String(protocolVersion)};`,
    "",
    enumBlocks.join("\n\n"),
    "",
    messageTypesDeclaration(),
    "",
    settingsDeclaration(),
    "",
    frameDeclaration(),
    "",
    "} // namespace orizon::protocol",
    "",
  ].join("\n");
}
