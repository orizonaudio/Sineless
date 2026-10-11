// The editor's minimum height, across every display mode and layout (from the protocol), every
// set of views on a mono or stereo track, and one to the most tracks, compared or not: each
// stacked trace drawn gets at least kMinimumLaneHeight above the chrome, and no more height is
// asked for than that.

#include "editor/Editor.h"

#include <algorithm>
#include <bit>
#include <gtest/gtest.h>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace orizon::editor {
namespace {

using protocol::DisplayMode;
using protocol::SettingId;
using protocol::ViewLayout;

struct Case
{
    DisplayMode mode = DisplayMode::kSingle;
    ViewLayout layout = ViewLayout::kStacked;
    int views = 0b0011; // Asked for.
    int channels = 2;
    int tracks = 1;
    bool compared = false; // Another track is checked in Compare.
};

std::vector<Case> everyCase()
{
    std::vector<Case> cases;
    for (std::size_t mode = 0; mode < protocol::kDisplayModeCount; ++mode) {
        for (std::size_t layout = 0; layout < protocol::kViewLayoutCount; ++layout) {
            const auto base = Case{.mode = static_cast<DisplayMode>(mode),
                                   .layout = static_cast<ViewLayout>(layout)};
            if (base.mode == DisplayMode::kSingle) {
                for (unsigned views = 1; views < (1U << protocol::kChannelViewCount); ++views) {
                    for (const int channels : {1, 2}) {
                        Case c = base;
                        c.views = static_cast<int>(views);
                        c.channels = channels;
                        cases.push_back(c);
                    }
                }
                continue;
            }
            for (int tracks = 1; std::cmp_less_equal(tracks, protocol::kMaxLanes); ++tracks) {
                for (const bool compared : {false, true}) {
                    if (compared && tracks < 2) {
                        continue; // Nothing else to compare with.
                    }
                    Case c = base;
                    c.tracks = tracks;
                    c.compared = compared;
                    cases.push_back(c);
                }
            }
        }
    }
    return cases;
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const Case& c, std::ostream* out)
{
    *out << protocol::kDisplayModeNames.at(static_cast<std::size_t>(c.mode)) << " "
         << protocol::kViewLayoutNames.at(static_cast<std::size_t>(c.layout)) << " views "
         << c.views << " channels " << c.channels << " tracks " << c.tracks
         << (c.compared ? " compared" : "");
}

// The traces the page draws for a case, as the product describes it: one per view on show (a
// mono track shows one), one per track, and the Compare sum once something is compared.
int tracesDrawn(const Case& c)
{
    switch (c.mode) {
    case DisplayMode::kSingle:
        return c.channels == 1 ? 1 : std::popcount(static_cast<unsigned>(c.views));
    case DisplayMode::kMulti:
        return c.tracks;
    default:
        return c.tracks + (c.compared ? 1 : 0);
    }
}

class MinimumHeight : public ::testing::TestWithParam<Case>
{
};

TEST_P(MinimumHeight, GivesEveryStackedTraceItsRoomAndNoMore)
{
    const Case c = GetParam();
    engine::Settings settings;
    settings.set(SettingId::kDisplayMode, static_cast<double>(c.mode));
    settings.set(SettingId::kViewLayout, static_cast<double>(c.layout));
    settings.set(SettingId::kChannelViews, static_cast<double>(c.views));
    engine::LaneSet lanes{.id = 1, .lanes = {}};
    for (int track = 0; track < c.tracks; ++track) {
        lanes.lanes.push_back({.id = static_cast<std::uint64_t>(track + 1),
                               .name = "",
                               .colour = std::nullopt,
                               .own = track == 0,
                               .compared = track == 0 || c.compared});
    }
    const int height = Editor::minimumHeightFor(settings, c.channels, lanes);
    EXPECT_GE(height, Editor::kMinimumHeight);
    const int traces = c.layout == ViewLayout::kStacked ? tracesDrawn(c) : 1;
    const int needed = Editor::kChromeHeight + traces * Editor::kMinimumLaneHeight;
    if (c.layout == ViewLayout::kStacked) {
        EXPECT_GE(height, needed) << traces << " traces";
    }
    EXPECT_LE(height, std::max(Editor::kMinimumHeight, needed)) << "more than the traces need";
}

INSTANTIATE_TEST_SUITE_P(Editor, MinimumHeight, ::testing::ValuesIn(everyCase()));

} // namespace
} // namespace orizon::editor
