#include "engine/FrameEncoder.h"

#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace orizon::engine {
namespace {

std::string toHex(const std::vector<std::byte>& bytes)
{
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::string hex;
    for (std::byte value : bytes) {
        const auto byte = static_cast<unsigned>(value);
        hex += kDigits[byte >> 4U];
        hex += kDigits[byte & 0xFU];
    }
    return hex;
}

// The golden frame is shared with the TypeScript decoder tests, so both sides agree byte for byte.
TEST(FrameEncoder, MatchesTheGoldenFrame)
{
    std::ifstream file(std::string(ORIZON_PROTOCOL_TEST_DATA_DIR) + "/golden-frame.json");
    const auto golden = nlohmann::json::parse(file);
    const auto& header = golden["header"];

    const FrameHeaderValues values{
        .sequence = header["sequence"].get<std::uint32_t>(),
        .mode = static_cast<protocol::FrameMode>(header["mode"].get<int>()),
        .flags = header["flags"].get<std::uint8_t>(),
        .views = header["views"].get<std::uint8_t>(),
        .pointCount = header["pointCount"].get<std::uint32_t>(),
        .samplesPerPoint = header["samplesPerPoint"].get<float>(),
        .startFraction = header["startFraction"].get<float>(),
        .peak = header["peak"].get<float>(),
        .rms = header["rms"].get<float>(),
        .frequency = header["frequency"].get<float>(),
        .cursor = header["cursor"].get<float>(),
        .cycleStart = header["cycleStart"].get<float>(),
        .spanQuarterNotes = header["spanQuarterNotes"].get<float>(),
        .barQuarterNotes = header["barQuarterNotes"].get<float>(),
    };
    std::vector<float> data = golden["views"]["left"].get<std::vector<float>>();
    const auto mid = golden["views"]["mid"].get<std::vector<float>>();
    data.insert(data.end(), mid.begin(), mid.end());

    std::vector<std::byte> output;
    encodeFrame(values, data, output);
    EXPECT_EQ(toHex(output), golden["hex"].get<std::string>());
}

TEST(FrameEncoder, WritesAHeaderOnlyFrame)
{
    std::vector<std::byte> output;
    encodeFrame({}, {}, output);
    EXPECT_EQ(output.size(), protocol::FrameHeader::kSize);
}

} // namespace
} // namespace orizon::engine
