#include "editor/Session.h"

#include <array>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace orizon::editor {
namespace {

using protocol::FrameHeader;
using protocol::SettingId;

class RecordingTransport final : public Transport
{
public:
    void sendMessage(const nlohmann::json& message) override { messages.push_back(message); }
    void sendFrame(std::span<const std::byte> /*frame*/) override {}

    [[nodiscard]] std::vector<nlohmann::json> ofType(const std::string& type) const
    {
        std::vector<nlohmann::json> found;
        for (const auto& message : messages) {
            if (message["type"] == type) {
                found.push_back(message);
            }
        }
        return found;
    }

    std::vector<nlohmann::json> messages;
};

// The examples are shared with the TypeScript protocol tests, so field names cannot drift.
nlohmann::json example(const std::string& name)
{
    std::ifstream file(std::string(ORIZON_PROTOCOL_TEST_DATA_DIR) + "/messages.json");
    return nlohmann::json::parse(file).at(name);
}

class SessionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        engine.setSampleRate(48000.0);
        // These tests use the seconds time base; Beats is the default.
        engine.settings().set(protocol::SettingId::kTimeBase,
                              static_cast<double>(protocol::TimeBase::kSeconds));
        std::vector<float> tone(48000);
        for (std::size_t i = 0; i < tone.size(); ++i) {
            tone[i] = (i / 50) % 2 == 0 ? 0.5f : -0.5f;
        }
        std::array<const float*, 2> channels{tone.data(), tone.data()};
        engine.captureRing().write(channels.data(), 2, static_cast<int>(tone.size()));
    }

    std::uint32_t producedPointCount()
    {
        std::vector<std::byte> frame;
        if (!analyzer.produce(frame)) {
            return 0;
        }
        std::uint32_t points = 0;
        std::memcpy(&points, frame.data() + FrameHeader::kPointCountOffset, sizeof(points));
        return points;
    }

    engine::Engine engine;
    engine::Analyzer analyzer{engine};
    RecordingTransport transport;
    Session session{engine, analyzer, transport};
};

TEST_F(SessionTest, AnswersHelloWithAFullSnapshot)
{
    session.receive(example("hello"));
    ASSERT_TRUE(session.started());
    const auto snapshots = transport.ofType("snapshot");
    ASSERT_EQ(snapshots.size(), 1u);
    const auto& snapshot = snapshots[0];
    EXPECT_EQ(snapshot["protocolVersion"], protocol::kProtocolVersion);
    EXPECT_EQ(snapshot["sampleRate"], 48000.0);
    EXPECT_EQ(snapshot["inputChannels"], 2);
    EXPECT_EQ(snapshot["settings"].size(), protocol::kSettingCount);
    EXPECT_EQ(snapshot["settings"]["triggerMode"], "auto");
    EXPECT_EQ(snapshot["settings"]["channelViews"], nlohmann::json::array({"left", "right"}));
}

TEST_F(SessionTest, HelloSetsTheTraceWidthInDevicePixels)
{
    engine.settings().set(SettingId::kTimeSpan, 0.1); // 4800 samples: more than the columns.
    session.receive(example("hello"));                // 640 CSS px at 2x.
    EXPECT_EQ(producedPointCount(), 1280u);
}

TEST_F(SessionTest, RejectsAProtocolMismatch)
{
    auto hello = example("hello");
    hello["protocolVersion"] = protocol::kProtocolVersion + 1;
    session.receive(hello);
    EXPECT_FALSE(session.started());
    const auto errors = transport.ofType("error");
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0]["code"], "protocolMismatch");
    EXPECT_TRUE(transport.ofType("snapshot").empty());
}

TEST_F(SessionTest, IgnoresMessagesBeforeHello)
{
    session.receive(example("setSetting"));
    EXPECT_TRUE(transport.messages.empty());
    EXPECT_EQ(engine.settings().get().value(SettingId::kTriggerMode), 0.0);
}

TEST_F(SessionTest, AppliesValidSettingsAndEchoesThem)
{
    session.receive(example("hello"));
    session.receive(example("setSetting"));
    session.receive(example("setSettingFlags"));
    session.receive(example("setSettingNumber"));
    EXPECT_EQ(engine.settings().get().value(SettingId::kTriggerMode), 1.0);
    EXPECT_EQ(engine.settings().get().value(SettingId::kChannelViews), 0b1100);
    EXPECT_EQ(engine.settings().get().value(SettingId::kTimeSpan), 0.5);
    const auto echoes = transport.ofType("stateChanged");
    ASSERT_EQ(echoes.size(), 3u);
    EXPECT_EQ(echoes[0]["name"], "triggerMode");
    EXPECT_EQ(echoes[0]["value"], "normal");
    EXPECT_EQ(echoes[1]["value"], nlohmann::json::array({"mid", "side"}));
}

TEST_F(SessionTest, AnswersAnInvalidValueWithTheValueKept)
{
    session.receive(example("hello"));
    session.receive({{"type", "setSetting"}, {"name", "timeSpan"}, {"value", 99.0}});
    const auto echoes = transport.ofType("stateChanged");
    ASSERT_EQ(echoes.size(), 1u);
    EXPECT_EQ(echoes[0]["value"], 1.0) << "the control snaps back to the stored value";
}

TEST_F(SessionTest, IgnoresUnknownSettingsAndMessageTypes)
{
    session.receive(example("hello"));
    transport.messages.clear();
    session.receive({{"type", "setSetting"}, {"name", "loudness"}, {"value", 1}});
    session.receive({{"type", "teleport"}});
    EXPECT_TRUE(transport.messages.empty());
}

TEST_F(SessionTest, ReportsHostStateChanges)
{
    session.receive(example("hello"));
    transport.messages.clear();
    engine::Settings restored = engine.settings().get();
    restored.set(SettingId::kTriggerLevel, 0.5);
    engine.settings().replace(restored);
    session.sync();
    const auto echoes = transport.ofType("stateChanged");
    ASSERT_EQ(echoes.size(), 1u) << "only the changed setting";
    EXPECT_EQ(echoes[0]["name"], "triggerLevel");
    session.sync();
    EXPECT_EQ(transport.ofType("stateChanged").size(), 1u) << "nothing new the second time";
}

// A project's rate, or the track's channels, can change with the editor open.
TEST_F(SessionTest, SendsTheSnapshotAgainWhenTheAudioFormatChanges)
{
    session.receive(example("hello"));
    transport.messages.clear();
    session.sync();
    EXPECT_TRUE(transport.ofType("snapshot").empty()) << "nothing changed";
    engine.setSampleRate(96000.0);
    session.sync();
    ASSERT_EQ(transport.ofType("snapshot").size(), 1u);
    EXPECT_EQ(transport.ofType("snapshot")[0]["sampleRate"], 96000.0);
    engine.setInputChannels(1);
    session.sync();
    ASSERT_EQ(transport.ofType("snapshot").size(), 2u);
    EXPECT_EQ(transport.ofType("snapshot")[1]["inputChannels"], 1);
}

// The multi-track view's lanes reach the page once, and again whenever they change.
TEST_F(SessionTest, ReportsTheMultiTrackLanes)
{
    engine.settings().set(protocol::SettingId::kTimeBase,
                          static_cast<double>(protocol::TimeBase::kBeats));
    engine.settings().set(protocol::SettingId::kBeatSpan,
                          static_cast<double>(protocol::BeatSpan::kQuarterBeat));
    engine.settings().set(protocol::SettingId::kDisplayMode,
                          static_cast<double>(protocol::DisplayMode::kMulti));
    engine.setTrackInfo({.name = "Kick", .colour = 0xe03030, .index = std::nullopt});
    session.receive(example("hello"));
    ASSERT_GT(producedPointCount(), 0u);
    session.sync();
    auto lanes = transport.ofType("lanes");
    ASSERT_EQ(lanes.size(), 1u);
    EXPECT_EQ(lanes[0]["laneSet"], analyzer.laneSet().id);
    EXPECT_EQ(lanes[0]["hidden"], 0);
    ASSERT_EQ(lanes[0]["lanes"].size(), 1u);
    EXPECT_EQ(lanes[0]["lanes"][0]["name"], "Kick");
    EXPECT_EQ(lanes[0]["lanes"][0]["colour"], 0xe03030);
    EXPECT_EQ(lanes[0]["lanes"][0]["own"], true);
    EXPECT_EQ(lanes[0]["lanes"][0]["id"], 0) << "an unregistered engine's own lane";
    EXPECT_EQ(lanes[0]["lanes"][0]["compared"], false) << "only the Compare view compares";

    session.sync();
    EXPECT_EQ(transport.ofType("lanes").size(), 1u) << "nothing new";
    engine.setTrackInfo({.name = "Kick 2", .colour = std::nullopt, .index = std::nullopt});
    producedPointCount();
    session.sync();
    lanes = transport.ofType("lanes");
    ASSERT_EQ(lanes.size(), 2u);
    EXPECT_EQ(lanes[1]["lanes"][0]["name"], "Kick 2");
    EXPECT_TRUE(lanes[1]["lanes"][0]["colour"].is_null());
}

TEST_F(SessionTest, ForwardsViewCommandsToTheAnalyzer)
{
    session.receive(example("hello"));
    session.receive(example("viewport")); // 500 CSS px at 1.5x.
    EXPECT_EQ(producedPointCount(), 750u);

    session.receive(example("freeze"));
    std::vector<std::byte> frame;
    ASSERT_TRUE(analyzer.produce(frame));
    std::uint8_t flags = 0;
    std::memcpy(&flags, frame.data() + FrameHeader::kFlagsOffset, sizeof(flags));
    EXPECT_NE(flags & static_cast<std::uint8_t>(protocol::FrameFlag::kFrozen), 0);
}

TEST_F(SessionTest, ForwardsTheViewToTheAnalyzer)
{
    engine.settings().set(SettingId::kTimeSpan, 0.1);
    session.receive(example("hello"));
    session.receive(example("view")); // A quarter of the span.
    std::vector<std::byte> frame;
    ASSERT_TRUE(analyzer.produce(frame));
    float visibleSamples = 0.0f;
    std::memcpy(&visibleSamples, frame.data() + FrameHeader::kVisibleSamplesOffset,
                sizeof(visibleSamples));
    EXPECT_EQ(visibleSamples, 1200.0f);
}

TEST_F(SessionTest, CanBeRestartedWithAnotherHello)
{
    session.receive(example("hello"));
    session.receive(example("hello"));
    EXPECT_EQ(transport.ofType("snapshot").size(), 2u);
}

} // namespace
} // namespace orizon::editor
