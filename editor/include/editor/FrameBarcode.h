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

} // namespace orizon::editor
