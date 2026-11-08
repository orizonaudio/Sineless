#include "editor/TraceView.h"

#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace orizon::editor {
namespace {

// The example shared with the TypeScript protocol tests, so field names cannot drift.
nlohmann::json example()
{
    std::ifstream file(std::string(ORIZON_PROTOCOL_TEST_DATA_DIR) + "/messages.json");
    return nlohmann::json::parse(file).at("traceView");
}

TEST(TraceViewMessage, ReadsEveryFieldOfThePagesMessage)
{
    const nlohmann::json message = example();
    const auto view = TraceView::parse(message);
    ASSERT_TRUE(view);
    EXPECT_EQ(view->view.start, message.at("viewStart").get<double>());
    EXPECT_EQ(view->view.end, message.at("viewEnd").get<double>());
    EXPECT_EQ(view->layout, protocol::ViewLayout::kStacked);
    EXPECT_EQ(view->verticalScaleDb, message.at("verticalScaleDb").get<double>());
    EXPECT_EQ(view->dim, message.at("dim").get<bool>());
    EXPECT_EQ(view->lineWidth, message.at("lineWidth").get<double>());
    EXPECT_EQ(view->emptyFrameLanes, message.at("emptyFrameLanes").get<std::size_t>());
    EXPECT_EQ(view->viewColours[2][0], message.at("viewColours")[2][0].get<float>());
    ASSERT_EQ(view->laneSets.size(), 1U);
    EXPECT_EQ(view->laneSets[0].laneSet, 7U);
    ASSERT_EQ(view->laneSets[0].lanes.size(), 2U);
    EXPECT_EQ(view->laneSets[0].lanes[1].id, 5);
    EXPECT_FALSE(view->laneSets[0].lanes[1].compared);
    EXPECT_EQ(view->fallbackColours.size(), message.at("fallbackColours").size());
    EXPECT_EQ(view->cancellation[3], message.at("cancellation")[3].get<float>());
    EXPECT_EQ(view->previous[3], message.at("previous")[3].get<float>());
    EXPECT_EQ(view->gridType, protocol::GridType::kNone);
    EXPECT_EQ(view->gridTime, protocol::GridDensity::kFine);
    EXPECT_EQ(view->gridLevel, protocol::GridDensity::kCoarse);
    EXPECT_EQ(view->beatDivision, protocol::BeatDivision::kTriplets);
}

// Every grid type the page can choose reaches the editor as chosen, so it draws the same lines.
TEST(TraceViewMessage, ReadsEveryGridType)
{
    for (std::size_t index = 0; index < protocol::kGridTypeNames.size(); ++index) {
        nlohmann::json message = example();
        message["gridChoice"]["type"] = protocol::kGridTypeNames[index];
        const auto view = TraceView::parse(message);
        ASSERT_TRUE(view) << protocol::kGridTypeNames[index];
        EXPECT_EQ(view->gridType, static_cast<protocol::GridType>(index));
    }
}

// A grid choice missing a part, or naming a value the protocol does not have, is refused: the
// editor keeps drawing the last good grid rather than one the page does not draw (QS-7).
TEST(TraceViewMessage, RefusesAGridChoiceItCannotRead)
{
    for (const char* part : {"type", "time", "level", "division"}) {
        nlohmann::json missing = example();
        missing["gridChoice"].erase(part);
        EXPECT_FALSE(TraceView::parse(missing)) << "without " << part;
        nlohmann::json unknown = example();
        unknown["gridChoice"][part] = "dense";
        EXPECT_FALSE(TraceView::parse(unknown)) << part << " unknown";
        nlohmann::json number = example();
        number["gridChoice"][part] = 1;
        EXPECT_FALSE(TraceView::parse(number)) << part << " as a number";
    }
}

// A message missing any field is refused, and the editor keeps the last good one (QS-7).
TEST(TraceViewMessage, RefusesAMessageMissingAnyField)
{
    const nlohmann::json fields = example();
    for (const auto& [name, value] : fields.items()) {
        if (name == "type") {
            continue;
        }
        nlohmann::json message = example();
        message.erase(name);
        EXPECT_FALSE(TraceView::parse(message)) << "without " << name;
    }
}

// Any field of the wrong type, not finite or out of range is refused.
TEST(TraceViewMessage, RefusesAMessageWithAnyFieldMalformed)
{
    const nlohmann::json fields = example();
    for (const auto& [name, value] : fields.items()) {
        if (name == "type") {
            continue;
        }
        nlohmann::json message = example();
        message[name] = "malformed";
        EXPECT_FALSE(TraceView::parse(message)) << name << " as a string";
    }
    const auto refuses = [](const std::function<void(nlohmann::json&)>& change, const char* what) {
        nlohmann::json message = example();
        change(message);
        EXPECT_FALSE(TraceView::parse(message)) << what;
    };
    refuses([](nlohmann::json& m) { m["viewStart"] = 0.8; }, "a view starting after it ends");
    refuses([](nlohmann::json& m) { m["viewEnd"] = 1.5; }, "a view past the span");
    refuses([](nlohmann::json& m) { m["viewStart"] = -0.1; }, "a view before the span");
    refuses([](nlohmann::json& m) { m["layout"] = "sideways"; }, "an unknown layout");
    refuses([](nlohmann::json& m) { m["lineWidth"] = 0; }, "no line width");
    refuses([](nlohmann::json& m) { m["lineWidth"] = 1000; }, "a line far too wide");
    refuses([](nlohmann::json& m) { m["screen"][1] = 1.5; }, "a colour channel above 1");
    refuses([](nlohmann::json& m) { m["grid"][0] = -0.1; }, "a colour channel below 0");
    refuses([](nlohmann::json& m) { m["axis"] = {0, 0, 0}; }, "a colour of three channels");
    refuses([](nlohmann::json& m) { m["viewColours"].erase(0); }, "three view colours");
    refuses([](nlohmann::json& m) { m["fallbackColours"] = nlohmann::json::array(); },
            "no fallback colours");
    refuses([](nlohmann::json& m) { m["laneSets"][0]["lanes"][0]["id"] = 1.5; }, "a lane id");
    refuses([](nlohmann::json& m) { m["laneSets"][0]["laneSet"] = -3; }, "a negative lane set");
    refuses(
        [](nlohmann::json& m) {
            m["laneSets"][0]["lanes"] = nlohmann::json::array();
            for (std::size_t lane = 0; lane < protocol::kMaxLanes + 2; ++lane) {
                m["laneSets"][0]["lanes"].push_back(
                    {{"id", lane}, {"colour", {0, 0, 0, 1}}, {"compared", false}});
            }
        },
        "more lanes than a frame holds");
    refuses(
        [](nlohmann::json& m) {
            for (int set = 0; set < 20; ++set) {
                m["laneSets"].push_back(m["laneSets"][0]);
            }
        },
        "far more lane sets than the page keeps");
}

// ----- Lane colours, as the page picks them -----

FrameView laneFrame(std::uint32_t laneSet, std::size_t lanes, bool compared)
{
    static const std::vector<float> kValues(4, 0.0f);
    FrameView frame;
    frame.laneSet = laneSet;
    frame.pointCount = 2;
    if (compared) {
        frame.flags = static_cast<std::uint8_t>(protocol::FrameFlag::kCompared);
    }
    for (std::size_t lane = 0; lane < lanes; ++lane) {
        frame.lanes.emplace_back(kValues);
    }
    return frame;
}

TEST(TraceViewColours, TakesTheFramesLaneSetThenTheNewestThenThePalette)
{
    const auto view = TraceView::parse(example());
    ASSERT_TRUE(view);
    const Rgba first = view->laneSets[0].lanes[0].colour;
    // The frame's own set.
    EXPECT_EQ(view->laneColours(laneFrame(7, 2, false)).at(0), first);
    // A set the page does not hold yet: the newest it holds.
    EXPECT_EQ(view->laneColours(laneFrame(8, 2, false)).at(0), first);
    // A set whose count does not match the frame's lanes: the palette by position.
    const auto byPosition = view->laneColours(laneFrame(7, 3, false));
    ASSERT_EQ(byPosition.size(), 3U);
    EXPECT_EQ(byPosition[2], view->fallbackColours[2 % view->fallbackColours.size()]);
}

// After a track change the page holds several lane sets; a frame of a set it has not sent yet takes
// the newest set's colours, in whatever order the sets were sent.
TEST(TraceViewColours, TakesTheNewestOfSeveralLaneSets)
{
    nlohmann::json message = example();
    nlohmann::json newer = message.at("laneSets")[0];
    newer["laneSet"] = 9U;
    newer["lanes"][0]["colour"] = {0.1, 0.2, 0.3, 1.0};
    const nlohmann::json older = message.at("laneSets")[0];
    for (const auto& sets :
         {nlohmann::json::array({older, newer}), nlohmann::json::array({newer, older})}) {
        message["laneSets"] = sets;
        const auto view = TraceView::parse(message);
        ASSERT_TRUE(view);
        EXPECT_EQ(view->laneColours(laneFrame(8, 2, false)).at(0), (Rgba{0.1f, 0.2f, 0.3f, 1.0f}))
            << "sets in the order " << sets[0].at("laneSet") << ", " << sets[1].at("laneSet");
    }
}

// In Compare the sum takes its own colour, compared tracks their own, and tracks left out are
// faint.
TEST(TraceViewColours, DrawsTracksLeftOutOfAComparisonFaint)
{
    const auto view = TraceView::parse(example());
    ASSERT_TRUE(view);
    const auto colours = view->laneColours(laneFrame(7, 3, true));
    ASSERT_EQ(colours.size(), 3U);
    EXPECT_EQ(colours[0], view->laneSets[0].lanes[0].colour) << "compared";
    EXPECT_FLOAT_EQ(colours[1][3], view->laneSets[0].lanes[1].colour[3] *
                                       static_cast<float>(protocol::kLeftOutAlpha))
        << "left out";
    EXPECT_EQ(colours[2], view->sumColour) << "the sum";
    EXPECT_EQ(view->comparedLanes(laneFrame(7, 3, true)), (std::vector<bool>{true, false, false}));
}

// ----- The screen's rectangle -----

// The screen's padding box, as the page lays it out: inside the toolbar, the status bar, the
// margins and the border; at the page's minimum width when the host holds the view narrower; hidden
// when the host holds it shorter than the chrome.
TEST(ScreenRectTest, PlacesTheScreenAsThePageLaysItOut)
{
    const double inset = protocol::kScreenMargin + protocol::kScreenBorder;
    const double chrome =
        protocol::kToolbarHeight + protocol::kStatusBarHeight + 2.0 * protocol::kScreenBorder;
    for (const double width : {static_cast<double>(protocol::kMinimumEditorWidth),
                               static_cast<double>(protocol::kDefaultEditorWidth), 1600.0}) {
        const ScreenRect rect = ScreenRect::of(width, 580.0);
        EXPECT_EQ(rect.x, inset);
        EXPECT_EQ(rect.y, protocol::kToolbarHeight + protocol::kScreenBorder);
        EXPECT_EQ(rect.width, width - 2.0 * inset);
        EXPECT_EQ(rect.height, 580.0 - chrome);
        EXPECT_TRUE(rect.visible);
    }
    const ScreenRect narrow = ScreenRect::of(800.0, 580.0);
    EXPECT_EQ(narrow.width, protocol::kMinimumEditorWidth - 2.0 * inset) << "the page's minimum";
    const ScreenRect shortRect = ScreenRect::of(1080.0, chrome - 10.0);
    EXPECT_EQ(shortRect.height, 1.0);
    EXPECT_FALSE(shortRect.visible);
}

} // namespace
} // namespace orizon::editor
