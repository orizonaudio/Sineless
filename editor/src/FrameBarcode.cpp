#include "editor/FrameBarcode.h"

namespace orizon::editor {

std::array<bool, kBarcodeRowCells> barcodeRow(std::uint64_t value)
{
    std::array<bool, kBarcodeRowCells> cells{};
    cells[0] = true;
    bool odd = false;
    for (std::size_t index = 0; index < protocol::kBarcodeDataBits; ++index) {
        const bool white = ((value >> (protocol::kBarcodeDataBits - 1 - index)) & 1U) != 0;
        cells[2 + index] = white;
        odd = odd != white;
    }
    cells[2 + protocol::kBarcodeDataBits] = odd;
    return cells;
}

} // namespace orizon::editor
