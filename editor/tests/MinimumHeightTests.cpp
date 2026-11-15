// The editor's minimum height, across every display mode with Compare off and on, and every layout
// (from the protocol), every
// set of views on a mono or stereo track, and one to the most tracks Multi shows: each
// stacked trace drawn gets at least kMinimumLaneHeight above the chrome, and no more height is
// asked for than that.

#include "Display.h"
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

using engine::Display;
using protocol::SettingId;
using protocol::ViewLayout;

struct Case
{
    Display mode = Display::kSingle;
    ViewLayout layout = ViewLayout::kStacked;
    int views = 0b0011; // Asked for.
    int channels = 2;
    int tracks = 1;
};

std::vector<Case> everyCase()
{
    std::vector<Case> cases;
    for (const Display mode : engine::everyDisplay()) {
        for (std::size_t layout = 0; layout < protocol::kViewLayoutCount; ++layout) {
            const auto base = Case{.mode = mode, .layout = static_cast<ViewLayout>(layout)};
            if (!engine::isMulti(base.mode)) {
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
            for (int tracks = 1; std::cmp_less_equal(tracks, protocol::kMaxShownTracks); ++tracks) {
                Case c = base;
                c.tracks = tracks;
                cases.push_back(c);
            }
        }
    }
    return cases;
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const Case& c, std::ostream* out)
{
    *out << engine::displayName(c.mode) << " "
         << protocol::kViewLayoutNames.at(static_cast<std::size_t>(c.layout)) << " views "
         << c.views << " channels " << c.channels << " tracks " << c.tracks;
}

// The traces the page draws for a case, as the analyzer draws them: one per view on show (a mono
// track shows one, also with the Compare switch on), one per track shown, and with Compare on the
// lane it adds once another track is shown with this one (see the analyzer test
// HasNoComparisonWithOnlyThisTrackShown).
int tracesDrawn(const Case& c)
{
    switch (c.mode) {
    case Display::kSingle:
    case Display::kSingleCompareOn:
        return c.channels == 1 ? 1 : std::popcount(static_cast<unsigned>(c.views));
    case Display::kMulti:
        return c.tracks;
    case Display::kCompare:
        return c.tracks + (c.tracks > 1 ? 1 : 0);
    }
    return 0;
}

class MinimumHeight : public ::testing::TestWithParam<Case>
{
};

TEST_P(MinimumHeight, GivesEveryStackedTraceItsRoomAndNoMore)
{
    const Case c = GetParam();
    engine::Settings settings;
    engine::show(settings, c.mode);
    settings.set(SettingId::kViewLayout, static_cast<double>(c.layout));
    settings.set(SettingId::kChannelViews, static_cast<double>(c.views));
    engine::LaneSet lanes{.id = 1, .lanes = {}};
    for (int track = 0; track < c.tracks; ++track) {
        lanes.lanes.push_back({.id = static_cast<std::uint64_t>(track + 1),
                               .name = "",
                               .colour = std::nullopt,
                               .own = track == 0});
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
