#include "editor/FrameBarcode.h"

#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>

namespace orizon::editor {
namespace {

// The rows the page draws for each value (ui/tests/frame-barcode.test.ts): the editor's strip must
// read back with the same decoder.
TEST(FrameBarcode, DrawsThePagesRowForEachValue)
{
    std::ifstream file(std::string(ORIZON_PROTOCOL_TEST_DATA_DIR) + "/frame-barcode.json");
    const nlohmann::json vectors = nlohmann::json::parse(file);
    ASSERT_FALSE(vectors.at("rows").empty());
    for (const nlohmann::json& row : vectors.at("rows")) {
        const auto value = row.at("value").get<std::uint64_t>();
        const auto cells = barcodeRow(value);
        const auto expected = row.at("cells").get<std::vector<int>>();
        ASSERT_EQ(cells.size(), expected.size()) << value;
        for (std::size_t index = 0; index < cells.size(); ++index) {
            EXPECT_EQ(cells[index] ? 1 : 0, expected[index]) << value << ", cell " << index;
        }
    }
}

} // namespace
} // namespace orizon::editor
