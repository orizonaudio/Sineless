#include "engine/FrameEncoder.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
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

nlohmann::json readGolden(const std::string& name)
{
    std::ifstream file(std::string(ORIZON_PROTOCOL_TEST_DATA_DIR) + "/" + name);
    return nlohmann::json::parse(file);
}

// A float of a golden file, where "NaN" stands for no value (JSON has no NaN).
float floatOf(const nlohmann::json& value)
{
    return value.is_string() && value.get<std::string>() == "NaN"
               ? std::numeric_limits<float>::quiet_NaN()
               : value.get<float>();
}

FrameHeaderValues headerValues(const nlohmann::json& header)
{
    return {
        .mode = static_cast<protocol::FrameMode>(header["mode"].get<int>()),
        .flags = header["flags"].get<std::uint8_t>(),
        .views = header["views"].get<std::uint8_t>(),
        .laneCount = header["laneCount"].get<std::uint8_t>(),
        .pointCount = header["pointCount"].get<std::uint32_t>(),
        .startFraction = header["startFraction"].get<float>(),
        .peak = header["peak"].get<float>(),
        .rms = header["rms"].get<float>(),
        .cursor = header["cursor"].get<double>(),
        .cycleStart = header["cycleStart"].get<double>(),
        .spanQuarterNotes = header["spanQuarterNotes"].get<float>(),
        .barQuarterNotes = header["barQuarterNotes"].get<float>(),
        .cycleSeconds = header["cycleSeconds"].get<double>(),
        .viewStart = header["viewStart"].get<double>(),
        .viewEnd = header["viewEnd"].get<double>(),
        .visibleSamples = header["visibleSamples"].get<float>(),
        .laneSet = header["laneSet"].get<std::uint32_t>(),
        .sumGain = header["sumGain"].get<float>(),
        .fixedGain = header["fixedGain"].get<float>(),
        .panReach = header["panReach"].get<float>(),
        .invalidSamples = header["invalidSamples"].get<std::uint32_t>(),
        .invalidLanes = header["invalidLanes"].get<std::uint32_t>(),
        .sampleRate = header["sampleRate"].get<float>(),
        .spanSeconds = header["spanSeconds"].get<float>(),
        .readoutView = static_cast<protocol::ChannelView>(header["readoutView"].get<int>()),
        .triggerView = static_cast<protocol::ChannelView>(header["triggerView"].get<int>()),
        .triggerPosition = floatOf(header["triggerPosition"]),
        .pointsPerSample = header["pointsPerSample"].get<std::uint32_t>(),
    };
}

// The golden frames are shared with the TypeScript tests, so both sides agree byte for byte.
TEST(FrameEncoder, MatchesTheGoldenFrame)
{
    const auto golden = readGolden("golden-frame.json");
    std::vector<float> data = golden["views"]["left"].get<std::vector<float>>();
    const auto mid = golden["views"]["mid"].get<std::vector<float>>();
    data.insert(data.end(), mid.begin(), mid.end());

    std::vector<std::byte> output;
    encodeFrame(headerValues(golden["header"]), data, output);
    EXPECT_EQ(toHex(output), golden["hex"].get<std::string>());
}

TEST(FrameEncoder, MatchesTheGoldenLanesFrame)
{
    const auto golden = readGolden("golden-lanes-frame.json");
    std::vector<float> data;
    for (const auto& lane : golden["lanes"]) {
        const auto values = lane.get<std::vector<float>>();
        data.insert(data.end(), values.begin(), values.end());
    }
    for (const auto& match : golden["matches"]) {
        data.push_back(floatOf(match["correlation"]));
        data.push_back(floatOf(match["offset"]));
        const bool inverted = match["inverted"].get<bool>();
        data.push_back(match["related"].get<bool>() ? (inverted ? -1.0f : 1.0f) : 0.0f);
    }

    std::vector<std::byte> output;
    encodeFrame(headerValues(golden["header"]), data, output);
    EXPECT_EQ(toHex(output), golden["hex"].get<std::string>());
}

TEST(FrameEncoder, WritesAHeaderOnlyFrame)
{
    std::vector<std::byte> output;
    encodeFrame({}, {}, output);
    EXPECT_EQ(output.size(), protocol::FrameHeader::kSize);
}

// Readouts not measured travel as no value, never as the level of silence.
TEST(FrameEncoder, SendsNoValueForReadoutsNotMeasured)
{
    std::vector<std::byte> output;
    encodeFrame({}, {}, output);
    for (const std::size_t offset :
         {protocol::FrameHeader::kPeakOffset, protocol::FrameHeader::kRmsOffset}) {
        float value = 0.0f;
        std::memcpy(&value, output.data() + offset, sizeof value);
        EXPECT_TRUE(std::isnan(value)) << "at byte " << offset;
    }
}

// A view of kMinimumViewSamples at the end of the longest sweep (2^23 samples), its playhead, and
// where a sweep starts late in a song, travel exactly:
// rounded to a float, its start would be off by a quarter of a sample and its width by about 2%,
// which the page's times and grid would read.
TEST(FrameEncoder, CarriesADeepViewExactly)
{
    const double span = 8388608.0;
    const double end = 1.0 - (3.0 / span);
    const double start = end - (protocol::kMinimumViewSamples / span) + 1e-13;
    std::vector<std::byte> output;
    // The playhead, on a sample near the end of 16 bars at 60 BPM and 96 kHz.
    const double cursor = 1.0 - (5.0 / 6144000.0);
    encodeFrame({.cursor = cursor,
                 .cycleStart = 4800.1,
                 .cycleSeconds = 2400.05,
                 .viewStart = start,
                 .viewEnd = end},
                {}, output);
    double carried = 0.0;
    std::memcpy(&carried, output.data() + protocol::FrameHeader::kViewStartOffset, sizeof carried);
    EXPECT_EQ(carried, start);
    std::memcpy(&carried, output.data() + protocol::FrameHeader::kViewEndOffset, sizeof carried);
    EXPECT_EQ(carried, end);
    // And where the sweep starts, late in a song, which the grid and clock are placed from.
    std::memcpy(&carried, output.data() + protocol::FrameHeader::kCycleStartOffset, sizeof carried);
    EXPECT_EQ(carried, 4800.1);
    std::memcpy(&carried, output.data() + protocol::FrameHeader::kCycleSecondsOffset,
                sizeof carried);
    EXPECT_EQ(carried, 2400.05);
    std::memcpy(&carried, output.data() + protocol::FrameHeader::kCursorOffset, sizeof carried);
    EXPECT_EQ(carried, cursor);
}

} // namespace
} // namespace orizon::engine
