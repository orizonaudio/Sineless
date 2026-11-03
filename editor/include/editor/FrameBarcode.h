#pragma once

#include "protocol/Protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace orizon::editor {

// The frame barcode's rows (protocol/src/limits.ts, barcodeDataBits), drawn while the diagnostic
// switch is on: the page's rows (ui/src/render/frame-barcode.ts), checked against the shared
// vectors in protocol/tests/data/frame-barcode.json.

// A white and a black reference, the value's cells, parity and the end.
inline constexpr std::size_t kBarcodeRowCells = protocol::kBarcodeDataBits + 4;

// The row for `value`, modulo the range: true is a white cell.
[[nodiscard]] std::array<bool, kBarcodeRowCells> barcodeRow(std::uint64_t value);

// What a picture's strip carries: its number since the switch was read, and the time it was drawn
// for in protocol::kBarcodeTicksPerMillisecond.
struct BarcodeValues
{
    std::uint64_t picture = 0;
    std::uint64_t drawnFor = 0;
};

// Where the strip's two rows go on a picture of the screen's size, in device pixels from its top
// left, as the page places them (barcodeStrip in ui/src/render/frame-barcode.ts).
struct BarcodeStrip
{
    double left = 0.0;
    double top = 0.0;
    double cell = 0.0;
};
[[nodiscard]] BarcodeStrip barcodeStrip(double width, double height, double pixelRatio);

} // namespace orizon::editor
