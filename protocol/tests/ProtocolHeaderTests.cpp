#include "protocol/Protocol.h"

#include <cstddef>
#include <gtest/gtest.h>

namespace orizon::protocol {
namespace {

TEST(ProtocolHeader, DescribesEverySettingInIdOrder)
{
    for (std::size_t index = 0; index < kSettings.size(); ++index) {
        EXPECT_EQ(static_cast<std::size_t>(kSettings[index].id), index);
        EXPECT_LE(kSettings[index].minimum, kSettings[index].defaultValue);
        EXPECT_LE(kSettings[index].defaultValue, kSettings[index].maximum);
    }
}

TEST(ProtocolHeader, MatchesTheApprovedSettingsTable)
{
    const auto& timeSpan = kSettings[static_cast<std::size_t>(SettingId::kTimeSpan)];
    EXPECT_EQ(timeSpan.name, "timeSpan");
    EXPECT_EQ(timeSpan.defaultValue, 0.02);
    EXPECT_TRUE(timeSpan.logarithmic);

    const auto& source = kSettings[static_cast<std::size_t>(SettingId::kTriggerSource)];
    EXPECT_EQ(source.kind, SettingKind::kEnum);
    EXPECT_EQ(source.defaultValue, static_cast<double>(ChannelView::kMid));
}

TEST(ProtocolHeader, LaysOutTheFrameHeaderWithNaturalAlignment)
{
    EXPECT_EQ(FrameHeader::kSequenceOffset, 0u);
    EXPECT_EQ(FrameHeader::kPointCountOffset % 4, 0u);
    EXPECT_EQ(FrameHeader::kSize, 16u);
}

TEST(ProtocolHeader, NamesMessageTypesInEnumOrder)
{
    EXPECT_EQ(kMessageTypeNames[static_cast<std::size_t>(MessageType::kSetSetting)], "setSetting");
}

} // namespace
} // namespace orizon::protocol
