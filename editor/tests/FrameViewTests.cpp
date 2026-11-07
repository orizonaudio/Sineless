#include "editor/FrameView.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace orizon::editor {
namespace {

// The golden frames, encoded independently of both implementations (protocol/tests/data).
nlohmann::json golden(const std::string& name)
{
    std::ifstream file(std::string(ORIZON_PROTOCOL_TEST_DATA_DIR) + "/" + name);
    return nlohmann::json::parse(file);
}

std::vector<std::byte> bytesOf(const std::string& hex)
{
    std::vector<std::byte> bytes;
    for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
        bytes.push_back(static_cast<std::byte>(std::stoi(hex.substr(index, 2), nullptr, 16)));
    }
    return bytes;
}

// What the native drawing reads from a frame matches what the page's decoder reads from the same
// bytes: the placing fields, the views in ChannelView order, the lanes and Compare's matches.
TEST(FrameView, ReadsTheGoldenFramesAsThePageDoes)
{
    {
        const nlohmann::json frame = golden("golden-frame.json");
        const auto bytes = bytesOf(frame.at("hex"));
        const auto read = FrameView::read(bytes);
        ASSERT_TRUE(read);
        const nlohmann::json& header = frame.at("header");
        EXPECT_EQ(read->mode, protocol::FrameMode::kMinMax);
        EXPECT_EQ(read->flags, header.at("flags").get<int>());
        EXPECT_EQ(read->pointCount, header.at("pointCount").get<std::uint32_t>());
        EXPECT_EQ(read->startFraction, header.at("startFraction").get<float>());
        EXPECT_EQ(read->viewStart, header.at("viewStart").get<float>());
        EXPECT_EQ(read->viewEnd, header.at("viewEnd").get<float>());
        EXPECT_EQ(read->visibleSamples, header.at("visibleSamples").get<float>());
        EXPECT_EQ(read->cursor, header.at("cursor").get<float>());
        // Where Cross hair's line goes, as the engine placed the trigger point.
        EXPECT_EQ(read->triggerPosition, header.at("triggerPosition").get<float>());
        ASSERT_EQ(read->views.size(), 2U);
        EXPECT_EQ(read->views[0].channelView, static_cast<int>(protocol::ChannelView::kLeft));
        EXPECT_EQ(read->views[1].channelView, static_cast<int>(protocol::ChannelView::kMid));
        EXPECT_EQ(std::vector<float>(read->views[1].values.begin(), read->views[1].values.end()),
                  frame.at("views").at("mid").get<std::vector<float>>());
        EXPECT_TRUE(read->lanes.empty());
    }
    {
        const nlohmann::json frame = golden("golden-lanes-frame.json");
        const auto bytes = bytesOf(frame.at("hex"));
        const auto read = FrameView::read(bytes);
        ASSERT_TRUE(read);
        const nlohmann::json& header = frame.at("header");
        EXPECT_EQ(read->mode, protocol::FrameMode::kRaw);
        EXPECT_TRUE(read->compared());
        EXPECT_EQ(read->laneSet, header.at("laneSet").get<std::uint32_t>());
        EXPECT_EQ(read->cycleStart, header.at("cycleStart").get<float>());
        EXPECT_EQ(read->spanQuarterNotes, header.at("spanQuarterNotes").get<float>());
        EXPECT_EQ(read->barQuarterNotes, header.at("barQuarterNotes").get<float>());
        // A sweep has no trigger point.
        EXPECT_TRUE(std::isnan(read->triggerPosition));
        ASSERT_EQ(read->lanes.size(), 3U);
        EXPECT_EQ(std::vector<float>(read->lanes[2].begin(), read->lanes[2].end()),
                  frame.at("lanes").at(2).get<std::vector<float>>());
        std::vector<bool> related;
        for (const nlohmann::json& match : frame.at("matches")) {
            related.push_back(match.at("related").get<bool>());
        }
        EXPECT_EQ(read->related, related);
    }
}

// Bytes shorter than the header announces are refused, as the page's decoder refuses them, at
// every length short of the whole frame: nothing is drawn from them.
TEST(FrameView, RefusesAFrameShorterThanItsHeaderAnnounces)
{
    for (const char* name : {"golden-frame.json", "golden-lanes-frame.json"}) {
        auto bytes = bytesOf(golden(name).at("hex"));
        for (std::size_t length = 0; length < bytes.size(); length += 4) {
            EXPECT_FALSE(FrameView::read(std::span(bytes).first(length))) << name << " " << length;
        }
        EXPECT_TRUE(FrameView::read(bytes));
    }
}

// Each frame says which lane set its lanes belong to, and the native editor colours them from it:
// a frozen frame from before a track joined keeps its own tracks' colours. Whatever the set's
// number, it is the one read.
TEST(FrameView, ReadsTheLaneSetTheFrameCarries)
{
    std::vector<std::byte> bytes =
        bytesOf(golden("golden-lanes-frame.json").at("hex").get<std::string>());
    for (const std::uint32_t laneSet : {0U, 7U, 123456U}) {
        std::memcpy(bytes.data() + protocol::FrameHeader::kLaneSetOffset, &laneSet, sizeof laneSet);
        const auto read = FrameView::read(bytes);
        ASSERT_TRUE(read);
        EXPECT_EQ(read->laneSet, laneSet);
    }
}

} // namespace
} // namespace orizon::editor
