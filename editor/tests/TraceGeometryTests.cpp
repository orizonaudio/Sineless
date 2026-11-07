#include "editor/TraceGeometry.h"

#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace orizon::editor {
namespace {

// The page's geometry, computed by its helpers (ui/tests/trace-geometry-vectors.test.ts): the
// native picture must place the same lanes, grid lines and shading.
const nlohmann::json& vectors()
{
    static const nlohmann::json kLoaded = [] {
        std::ifstream file(std::string(ORIZON_PROTOCOL_TEST_DATA_DIR) + "/trace-geometry.json");
        return nlohmann::json::parse(file);
    }();
    return kLoaded;
}

protocol::ViewLayout layoutNamed(const std::string& name)
{
    return name == "stacked" ? protocol::ViewLayout::kStacked : protocol::ViewLayout::kOverlaid;
}

TEST(TraceGeometry, PlacesLanesAsThePageDoes)
{
    for (const nlohmann::json& entry : vectors().at("lanes")) {
        const auto placed =
            lanes(entry.at("count").get<std::size_t>(), layoutNamed(entry.at("layout")));
        const nlohmann::json& expected = entry.at("lanes");
        ASSERT_EQ(placed.size(), expected.size()) << entry.dump();
        for (std::size_t index = 0; index < placed.size(); ++index) {
            EXPECT_NEAR(placed[index].center, expected[index].at("center").get<double>(), 1e-9);
            EXPECT_NEAR(placed[index].halfHeight, expected[index].at("halfHeight").get<double>(),
                        1e-9);
        }
    }
}

TEST(TraceGeometry, FitsLevelLinesAsThePageDoes)
{
    for (const nlohmann::json& entry : vectors().at("levelSteps")) {
        EXPECT_EQ(levelSteps(entry.at("halfLane").get<double>(), protocol::kGridMediumLineGap),
                  entry.at("steps").get<std::vector<int>>())
            << entry.dump();
    }
}

protocol::GridDensity densityNamed(const std::string& name)
{
    const auto found = std::ranges::find(protocol::kGridDensityNames, name);
    EXPECT_NE(found, protocol::kGridDensityNames.end()) << name;
    return static_cast<protocol::GridDensity>(found - protocol::kGridDensityNames.begin());
}

protocol::GridType typeNamed(const std::string& name)
{
    const auto found = std::ranges::find(protocol::kGridTypeNames, name);
    EXPECT_NE(found, protocol::kGridTypeNames.end()) << name;
    return static_cast<protocol::GridType>(found - protocol::kGridTypeNames.begin());
}

TEST(TraceGeometry, DrawsLevelLinesAtEachLevelStepAsThePageDoes)
{
    for (const nlohmann::json& entry : vectors().at("levelLines")) {
        const auto lines =
            levelLines(entry.at("halfLane").get<double>(), densityNamed(entry.at("density")));
        const nlohmann::json& expected = entry.at("lines");
        ASSERT_EQ(lines.size(), expected.size()) << entry.dump();
        for (std::size_t index = 0; index < lines.size(); ++index) {
            EXPECT_NEAR(lines[index].step, expected[index].at("step").get<double>(), 1e-9);
            EXPECT_EQ(lines[index].strong, expected[index].at("strong").get<bool>());
        }
    }
}

TEST(TraceGeometry, PlacesMusicalLinesAsThePageDoes)
{
    for (const nlohmann::json& entry : vectors().at("musical")) {
        const auto division = std::ranges::find(protocol::kBeatDivisionNames,
                                                entry.at("division").get<std::string>());
        ASSERT_NE(division, protocol::kBeatDivisionNames.end()) << entry.dump();
        const auto lines =
            musicalGridLines(entry.at("cycleStart").get<double>(), entry.at("span").get<double>(),
                             entry.at("bar").get<double>(),
                             {.start = entry.at("viewStart").get<double>(),
                              .end = entry.at("viewEnd").get<double>()},
                             {.time = densityNamed(entry.at("time")),
                              .division = static_cast<protocol::BeatDivision>(
                                  division - protocol::kBeatDivisionNames.begin()),
                              .widthCss = entry.at("widthCss").get<double>()});
        const nlohmann::json& expected = entry.at("lines");
        ASSERT_EQ(lines.size(), expected.size()) << entry.dump();
        for (std::size_t index = 0; index < lines.size(); ++index) {
            EXPECT_NEAR(lines[index].position, expected[index].at("position").get<double>(), 1e-9);
            EXPECT_EQ(lines[index].strong, expected[index].at("strong").get<bool>());
        }
    }
}

TEST(TraceGeometry, DrawsTheGridAsThePageDoes)
{
    for (const nlohmann::json& entry : vectors().at("grid")) {
        std::vector<VerticalLine> vertical;
        if (entry.contains("vertical")) {
            for (const nlohmann::json& line : entry.at("vertical")) {
                vertical.push_back(
                    {.position = line.at("position").get<double>(), .strong = line.at("strong")});
            }
        }
        const GridLines lines =
            gridLines(entry.at("width").get<double>(), entry.at("height").get<double>(),
                      entry.at("laneCount").get<std::size_t>(), entry.at("ratio").get<double>(),
                      {.type = typeNamed(entry.at("type")),
                       .level = densityNamed(entry.at("level")),
                       .columns = gridColumns(densityNamed(entry.at("time"))),
                       .vertical = entry.contains("vertical") ? &vertical : nullptr,
                       .trigger = entry.value("trigger", -1.0)});
        const nlohmann::json& expected = entry.at("lines");
        EXPECT_EQ(lines.lineWidth, expected.at("lineWidth").get<double>()) << entry.dump();
        ASSERT_EQ(lines.vertical.size(), expected.at("vertical").size()) << entry.dump();
        for (std::size_t index = 0; index < lines.vertical.size(); ++index) {
            EXPECT_EQ(lines.vertical[index].x,
                      expected.at("vertical")[index].at("x").get<double>());
            EXPECT_EQ(lines.vertical[index].strong,
                      expected.at("vertical")[index].at("strong").get<bool>());
        }
        ASSERT_EQ(lines.ticks.size(), expected.at("ticks").size()) << entry.dump();
        for (std::size_t index = 0; index < lines.ticks.size(); ++index) {
            const nlohmann::json& tick = expected.at("ticks")[index];
            EXPECT_EQ(lines.ticks[index].x0, tick.at("x0").get<double>());
            EXPECT_EQ(lines.ticks[index].y0, tick.at("y0").get<double>());
            EXPECT_EQ(lines.ticks[index].x1, tick.at("x1").get<double>());
            EXPECT_EQ(lines.ticks[index].y1, tick.at("y1").get<double>());
            EXPECT_EQ(lines.ticks[index].strong, tick.at("strong").get<bool>());
        }
        ASSERT_EQ(lines.horizontal.size(), expected.at("horizontal").size()) << entry.dump();
        for (std::size_t index = 0; index < lines.horizontal.size(); ++index) {
            EXPECT_EQ(lines.horizontal[index].y,
                      expected.at("horizontal")[index].at("y").get<double>());
            EXPECT_EQ(lines.horizontal[index].axis,
                      expected.at("horizontal")[index].at("axis").get<bool>());
        }
    }
}

// A lane's values as the vectors hold them: JSON has no NaN, so a missing point is null.
std::vector<float> laneValues(const nlohmann::json& lane)
{
    std::vector<float> values;
    values.reserve(lane.size());
    for (const nlohmann::json& value : lane) {
        values.push_back(value.is_null() ? std::numeric_limits<float>::quiet_NaN()
                                         : value.get<float>());
    }
    return values;
}

TEST(TraceGeometry, ShadesCancellationAsThePageDoes)
{
    for (const nlohmann::json& entry : vectors().at("cancellation")) {
        std::vector<std::vector<float>> data;
        for (const nlohmann::json& lane : entry.at("lanes")) {
            data.push_back(laneValues(lane));
        }
        FrameView frame;
        frame.mode =
            entry.at("mode") == "minMax" ? protocol::FrameMode::kMinMax : protocol::FrameMode::kRaw;
        frame.flags = static_cast<std::uint8_t>(protocol::FrameFlag::kCompared);
        frame.pointCount = entry.at("pointCount").get<std::uint32_t>();
        frame.startFraction = entry.at("startFraction").get<float>();
        frame.viewStart = entry.at("frameViewStart").get<float>();
        frame.viewEnd = entry.at("frameViewEnd").get<float>();
        frame.visibleSamples = entry.at("visibleSamples").get<float>();
        for (const auto& lane : data) {
            frame.lanes.emplace_back(lane);
        }
        frame.related = entry.at("related").get<std::vector<bool>>();
        const auto bands = cancellationBands(frame, layoutNamed(entry.at("layout")),
                                             entry.at("compared").get<std::vector<bool>>(),
                                             {.start = entry.at("viewStart").get<double>(),
                                              .end = entry.at("viewEnd").get<double>()});
        const auto expected = entry.at("bands").get<std::vector<double>>();
        // The page's quads: two triangles, 12 values each, (x0, bottom, x1, bottom, x0, top, ...).
        ASSERT_EQ(bands.size() * 12, expected.size()) << entry.dump();
        for (std::size_t index = 0; index < bands.size(); ++index) {
            const double* quad = &expected[index * 12];
            // The page's values are float32: compare at that precision.
            EXPECT_NEAR(bands[index].left, quad[0], 1e-5);
            EXPECT_NEAR(bands[index].bottom, quad[1], 1e-5);
            EXPECT_NEAR(bands[index].right, quad[2], 1e-5);
            EXPECT_NEAR(bands[index].top, quad[5], 1e-5);
        }
    }
}

// A track that matches this one nowhere clearly has nothing to fix: no shading, as on the page.
TEST(TraceGeometry, ShadesNothingWithoutARelatedComparedTrack)
{
    const nlohmann::json& entry = vectors().at("cancellation").at(0);
    std::vector<std::vector<float>> data;
    for (const nlohmann::json& lane : entry.at("lanes")) {
        data.push_back(laneValues(lane));
    }
    FrameView frame;
    frame.mode = protocol::FrameMode::kMinMax;
    frame.flags = static_cast<std::uint8_t>(protocol::FrameFlag::kCompared);
    frame.pointCount = entry.at("pointCount").get<std::uint32_t>();
    for (const auto& lane : data) {
        frame.lanes.emplace_back(lane);
    }
    frame.related = {false, false};
    EXPECT_TRUE(cancellationBands(frame, protocol::ViewLayout::kStacked, {true, true}, {}).empty());
}

// Drawn natively, the playhead moves by fractions of a pixel: at any position its coverage over
// the pixels it touches adds up to its width, and it sits centred where it should, so neither its
// brightness nor its place steps with the pixel grid.
TEST(TraceGeometry, DrawsThePlayheadAtItsExactPositionWithCoverage)
{
    for (const double ratio : {1.0, 2.0, 1.5}) {
        // Positions across a few pixels, at fractions of a pixel.
        for (int step = 0; step < 28; ++step) {
            const double position = 0.30 + step * 0.00037;
            const PixelBand band = playheadBand(position, 2000.0, ratio);
            const double width = std::max(1.0, std::floor(ratio));
            EXPECT_NEAR(band.right - band.left, width, 1e-9);
            EXPECT_NEAR((band.left + band.right) / 2.0, position * 2000.0, 1e-9);
            double covered = 0.0;
            double weighted = 0.0;
            const auto first = static_cast<int>(std::floor(band.left)) - 1;
            const auto last = static_cast<int>(std::ceil(band.right)) + 1;
            for (int column = first; column <= last; ++column) {
                const auto pixel = static_cast<double>(column);
                const double coverage = bandCoverage(pixel, band);
                covered += coverage;
                weighted += coverage * (pixel + 0.5);
            }
            EXPECT_NEAR(covered, width, 1e-9);
            EXPECT_NEAR(weighted / covered, position * 2000.0, 1e-9);
        }
    }
}

} // namespace
} // namespace orizon::editor
