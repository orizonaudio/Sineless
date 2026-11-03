#include "editor/FrameBarcode.h"

#include <algorithm>
#include <cmath>

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

BarcodeStrip barcodeStrip(double width, double height, double pixelRatio)
{
    const auto inset = static_cast<double>(
        std::lround((static_cast<double>(protocol::kScreenRadius) + 2.0) * pixelRatio));
    const auto cell = static_cast<double>(protocol::kBarcodeCellPixels);
    return {.left = std::max(0.0, width - inset - static_cast<double>(kBarcodeRowCells) * cell),
            .top = std::min(inset, std::max(0.0, height - 2.0 * cell)),
            .cell = cell};
}

} // namespace orizon::editor
