#include "editor/TraceGeometry.h"
#include "mac/MetalTraceRenderer.h"

#import <Metal/Metal.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <zlib.h>

namespace orizon::editor {
namespace {

// The page's pictures (ui/tests/app/trace-pictures.test.ts): scenes drawn by its WebGL renderer in
// WebKit, with their frames and trace views.
const nlohmann::json& pictures()
{
    static const nlohmann::json kLoaded = [] {
        std::ifstream file(std::string(ORIZON_PROTOCOL_TEST_DATA_DIR) + "/trace-pictures.json");
        return nlohmann::json::parse(file);
    }();
    return kLoaded;
}

std::vector<std::byte> unbase64(const std::string& text)
{
    const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        return c == '+' ? 62 : c == '/' ? 63 : -1;
    };
    std::vector<std::byte> out;
    unsigned bits = 0;
    int count = 0;
    for (const char c : text) {
        const int v = value(c);
        if (v < 0) {
            continue;
        }
        bits = (bits << 6U) | static_cast<unsigned>(v);
        count += 6;
        if (count >= 8) {
            count -= 8;
            out.push_back(static_cast<std::byte>((bits >> static_cast<unsigned>(count)) & 0xFFU));
        }
    }
    return out;
}

std::vector<std::uint8_t> gunzip(const std::vector<std::byte>& bytes, std::size_t size)
{
    std::vector<std::uint8_t> out(size);
    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(bytes.data()));
    stream.avail_in = static_cast<uInt>(bytes.size());
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    EXPECT_EQ(inflateInit2(&stream, 16 + MAX_WBITS), Z_OK);
    EXPECT_EQ(inflate(&stream, Z_FINISH), Z_STREAM_END);
    EXPECT_EQ(stream.total_out, size);
    inflateEnd(&stream);
    return out;
}

struct Scene
{
    std::string name;
    std::size_t width = 0;
    std::size_t height = 0;
    double ratio = 1.0;
    std::vector<std::byte> bytes;
    TraceView view;
    // Premultiplied RGBA rows from the bottom, as WebGL reads them.
    std::vector<std::uint8_t> pixels;
    // The frame barcode's values, where the scene has the diagnostic switch on.
    std::optional<BarcodeValues> barcode;

    [[nodiscard]] FrameView frame() const { return *FrameView::read(bytes); }
};

std::vector<Scene> scenes()
{
    std::vector<Scene> out;
    for (const nlohmann::json& entry : pictures().at("scenes")) {
        Scene scene;
        scene.name = entry.at("name").get<std::string>();
        scene.width = entry.at("width").get<std::size_t>();
        scene.height = entry.at("height").get<std::size_t>();
        scene.ratio = entry.at("ratio").get<double>();
        scene.bytes = unbase64(entry.at("frame").get<std::string>());
        const auto view = TraceView::parse(entry.at("traceView"));
        EXPECT_TRUE(view) << scene.name;
        scene.view = view.value_or(TraceView{});
        scene.pixels =
            gunzip(unbase64(entry.at("pixels").get<std::string>()), scene.width * scene.height * 4);
        EXPECT_TRUE(FrameView::read(scene.bytes)) << scene.name;
        if (entry.contains("barcode")) {
            scene.barcode =
                BarcodeValues{.picture = entry.at("barcode").at("picture").get<std::uint64_t>(),
                              .drawnFor = entry.at("barcode").at("drawnFor").get<std::uint64_t>()};
        }
        out.push_back(std::move(scene));
    }
    return out;
}

// A picture read back from the GPU: premultiplied RGBA rows from the bottom, as the page's.
struct Picture
{
    std::size_t width = 0;
    std::size_t height = 0;
    std::vector<std::uint8_t> pixels;

    [[nodiscard]] const std::uint8_t* at(std::size_t x, std::size_t y) const
    {
        return &pixels[(y * width + x) * 4];
    }
};

class MetalTraceRendererTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            GTEST_SKIP() << "No Metal device: the picture parity tests need one.";
        }
        queue = [device newCommandQueue];
        renderer = MetalTraceRenderer::create(device);
        ASSERT_NE(renderer, nullptr);
    }

    // Draws into a drawable-like texture of the given size and reads it back.
    std::optional<Picture> draw(std::size_t width, std::size_t height, const FrameView* frame,
                                const TraceView& view, double ratio, bool grid = false,
                                const BarcodeValues* barcode = nullptr)
    {
        MTLTextureDescriptor* descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:width
                                                              height:height
                                                           mipmapped:NO];
        descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        descriptor.storageMode = MTLStorageModePrivate;
        id<MTLTexture> target = [device newTextureWithDescriptor:descriptor];
        id<MTLBuffer> readback = [device newBufferWithLength:width * height * 4
                                                     options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> buffer = [queue commandBuffer];
        const auto result = renderer->encode(buffer, target, frame, view, ratio, grid, barcode);
        if (result != MetalTraceRenderer::Result::kDrawn) {
            [buffer commit];
            return std::nullopt;
        }
        id<MTLBlitCommandEncoder> blit = [buffer blitCommandEncoder];
        [blit copyFromTexture:target
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(width, height, 1)
                            toBuffer:readback
                   destinationOffset:0
              destinationBytesPerRow:width * 4
            destinationBytesPerImage:width * height * 4];
        [blit endEncoding];
        [buffer commit];
        [buffer waitUntilCompleted];
        EXPECT_EQ(buffer.status, MTLCommandBufferStatusCompleted) << buffer.error;
        Picture picture{.width = width, .height = height, .pixels = {}};
        picture.pixels.resize(width * height * 4);
        const auto* bgra = static_cast<const std::uint8_t*>(readback.contents);
        for (std::size_t y = 0; y < height; ++y) {
            // Metal's rows count from the top.
            const std::uint8_t* row = bgra + (height - 1 - y) * width * 4;
            for (std::size_t x = 0; x < width; ++x) {
                std::uint8_t* out = &picture.pixels[(y * width + x) * 4];
                out[0] = row[x * 4 + 2];
                out[1] = row[x * 4 + 1];
                out[2] = row[x * 4 + 0];
                out[3] = row[x * 4 + 3];
            }
        }
        return picture;
    }

    std::optional<Picture> draw(const Scene& scene)
    {
        const FrameView frame = scene.frame();
        return draw(scene.width, scene.height, &frame, scene.view, scene.ratio, false,
                    scene.barcode ? &*scene.barcode : nullptr);
    }

    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    std::unique_ptr<MetalTraceRenderer> renderer;
};

// The pixel columns where the editor deliberately differs from the page: the moving edges it draws
// at their exact positions with coverage, which the page snaps to whole pixels (AD-30). The
// playhead's are checked by their coverage below.
std::vector<bool> movingEdgeColumns(const Scene& scene)
{
    std::vector<bool> out(scene.width, false);
    const auto mark = [&](double from, double to) {
        for (auto column = static_cast<long>(std::floor(from));
             column < static_cast<long>(std::ceil(to)); ++column) {
            if (column >= 0 && std::cmp_less(column, scene.width)) {
                out[static_cast<std::size_t>(column)] = true;
            }
        }
    };
    const FrameView frame = scene.frame();
    const auto width = static_cast<double>(scene.width);
    for (const Band& band : cancellationBands(frame, scene.view.layout,
                                              scene.view.comparedLanes(frame), scene.view.view)) {
        for (const double edge : {band.left, band.right}) {
            const double pixels = (edge + 1.0) / 2.0 * width;
            mark(pixels, pixels + 1.0);
        }
    }
    if (frame.has(protocol::FrameFlag::kSynced) && frame.cursor >= 0.0f) {
        const double position =
            (frame.cursor - scene.view.view.start) / (scene.view.view.end - scene.view.view.start);
        const PixelBand line = playheadBand(position, width, scene.ratio);
        // The page's snapped line may sit a pixel either side.
        mark(line.left - 1.0, line.right + 1.0);
    }
    return out;
}

void expectSamePicture(const Scene& scene, const Picture& drawn)
{
    ASSERT_EQ(drawn.pixels.size(), scene.pixels.size()) << scene.name;
    const std::vector<bool> skipped = movingEdgeColumns(scene);
    int worst = 0;
    std::size_t different = 0;
    std::string first;
    for (std::size_t y = 0; y < scene.height; ++y) {
        for (std::size_t x = 0; x < scene.width; ++x) {
            if (skipped[x]) {
                continue;
            }
            const std::uint8_t* expected = &scene.pixels[(y * scene.width + x) * 4];
            const std::uint8_t* actual = drawn.at(x, y);
            int difference = 0;
            for (int channel = 0; channel < 4; ++channel) {
                difference = std::max(difference, std::abs(expected[channel] - actual[channel]));
            }
            // A level per channel: the two GPUs' float arithmetic rounds differently.
            if (difference > 1) {
                if (different == 0) {
                    first = "(" + std::to_string(x) + ", " + std::to_string(y) + ") page " +
                            std::to_string(expected[0]) + "," + std::to_string(expected[1]) + "," +
                            std::to_string(expected[2]) + "," + std::to_string(expected[3]) +
                            " editor " + std::to_string(actual[0]) + "," +
                            std::to_string(actual[1]) + "," + std::to_string(actual[2]) + "," +
                            std::to_string(actual[3]);
                }
                ++different;
            }
            worst = std::max(worst, difference);
        }
    }
    EXPECT_EQ(different, 0U) << scene.name << ": worst " << worst << ", first at " << first;
}

// Every mode, time base and channel layout, Compare's shading, dim and a zoom's stretch: the
// editor draws the page's picture.
TEST_F(MetalTraceRendererTest, DrawsThePagesPictureOfEveryScene)
{
    for (const Scene& scene : scenes()) {
        const auto drawn = draw(scene);
        ASSERT_TRUE(drawn) << scene.name;
        expectSamePicture(scene, *drawn);
    }
}

// One renderer handed drawables of changing sizes (larger, smaller, the same, back and forth) and
// frames whose lanes come and go sizes its targets to each and draws each scene's own lanes.
TEST_F(MetalTraceRendererTest, FollowsEachDrawablesSizeAndEachFramesLanes)
{
    std::vector<Scene> all = scenes();
    std::vector<const Scene*> order;
    for (const Scene& scene : all) {
        order.push_back(&scene);
        order.push_back(&scene);
    }
    for (const Scene& scene : std::views::reverse(all)) {
        order.push_back(&scene);
    }
    for (const Scene* scene : order) {
        const auto drawn = draw(*scene);
        ASSERT_TRUE(drawn) << scene->name;
        expectSamePicture(*scene, *drawn);
    }
}

// Drawn natively, the playhead moves by fractions of a pixel: at every position the light it gives
// off across a row adds up to its width and centres on its position, so it neither flashes as it
// crosses pixels (a line split over two pixels as bright as one on a pixel) nor steps.
TEST_F(MetalTraceRendererTest, DrawsThePlayheadAtItsExactPosition)
{
    TraceView view = scenes().front().view;
    view.previous = {0.0f, 0.0f, 0.0f, 0.0f};
    view.cursor = {1.0f, 1.0f, 1.0f, 1.0f};
    // The light a value gives off: sRGB decoded (IEC 61966-2-1).
    const auto light = [](double value) {
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    // Over nothing, as the parity scenes draw, and over the editor's own dark screen.
    for (const Rgba& screen :
         {Rgba{0.0f, 0.0f, 0.0f, 0.0f}, Rgba{3 / 255.0f, 3 / 255.0f, 4 / 255.0f, 1.0f}}) {
        view.screen = screen;
        const double background = light(screen[0]) * screen[3];
        for (const double ratio : {1.0, 2.0}) {
            const std::size_t width = ratio == 1.0 ? 200 : 400;
            for (const double position : {0.25, 0.2513, 0.2537, 0.2561, 0.2589}) {
                FrameView frame;
                frame.flags = static_cast<std::uint8_t>(protocol::FrameFlag::kSynced);
                frame.cursor = static_cast<float>(position);
                const auto drawn = draw(width, 20, &frame, view, ratio);
                ASSERT_TRUE(drawn);
                double covered = 0.0;
                double weighted = 0.0;
                for (std::size_t x = 0; x < width; ++x) {
                    // The line's light: what the pixel gives off beyond the screen's.
                    const double alpha = light(drawn->at(x, 10)[0] / 255.0) - background;
                    covered += alpha;
                    weighted += alpha * (static_cast<double>(x) + 0.5);
                }
                const double lineWidth = std::max(1.0, std::floor(ratio));
                // Within the rounding of the few 8-bit pixels it touches.
                EXPECT_NEAR(covered, lineWidth, 0.02)
                    << position << " at " << ratio << " over " << screen[3];
                EXPECT_NEAR(weighted / covered, position * static_cast<double>(width), 0.02)
                    << position << " at " << ratio << " over " << screen[3];
            }
        }
    }
}

// Drawn hard one pixel wide (a 1x display), the playhead's brightest pixel would give off twice the
// light on a pixel as between two, flashing as it moves. At every display scale its brightest pixel
// stays near as bright wherever it sits.
TEST_F(MetalTraceRendererTest, KeepsThePlayheadEquallyBrightWhereverItSits)
{
    TraceView view = scenes().front().view;
    view.previous = {0.0f, 0.0f, 0.0f, 0.0f};
    view.cursor = {1.0f, 1.0f, 1.0f, 1.0f};
    for (const double ratio : {1.0, 2.0}) {
        const auto width = static_cast<std::size_t>(200 * ratio);
        double brightest = 0.0;
        double dimmest = 1.0;
        for (int step = 0; step <= 20; ++step) {
            FrameView frame;
            frame.flags = static_cast<std::uint8_t>(protocol::FrameFlag::kSynced);
            // Across one whole device pixel, in twentieths.
            frame.cursor = static_cast<float>((50.0 + step / 20.0) / static_cast<double>(width));
            const auto drawn = draw(width, 20, &frame, view, ratio);
            ASSERT_TRUE(drawn);
            double peak = 0.0;
            for (std::size_t x = 0; x < width; ++x) {
                const double value = drawn->at(x, 10)[0] / 255.0;
                peak = std::max(peak, value <= 0.04045 ? value / 12.92
                                                       : std::pow((value + 0.055) / 1.055, 2.4));
            }
            brightest = std::max(brightest, peak);
            dimmest = std::min(dimmest, peak);
        }
        // A hard one-pixel line's peak halves between two pixels; well under that.
        EXPECT_LT(brightest / dimmest, 1.5) << "at " << ratio;
    }
}

// The grid under the traces, where the page strokes it: the Time step's divisions, or a Beats
// sweep's beats and bars, and each lane's level and zero lines at the Level step, over the screen's
// colour, for the type and steps the page's trace view names; Cross hair (Trig only) its zero
// lines, the trigger point's line and their ticks; None its zero lines alone. A Beats sweep with no
// tempo yet (the host not yet played: FR-9.1) has no beats to show, and shows the divisions as the
// page does.
TEST_F(MetalTraceRendererTest, DrawsTheGridWhereThePageDoes)
{
    TraceView view = scenes().front().view;
    view.screen = {0.0f, 0.0f, 0.0f, 1.0f};
    view.grid = {0.0f, 1.0f, 0.0f, 1.0f};
    view.axis = {0.0f, 0.0f, 1.0f, 1.0f};
    view.layout = protocol::ViewLayout::kStacked;
    using protocol::GridDensity;
    using protocol::GridType;
    for (const auto& [type, time, level] :
         std::vector<std::tuple<GridType, GridDensity, GridDensity>>{
             {GridType::kGrid, GridDensity::kMedium, GridDensity::kMedium},
             {GridType::kGrid, GridDensity::kCoarse, GridDensity::kFine},
             {GridType::kGrid, GridDensity::kFine, GridDensity::kCoarse},
             {GridType::kCrossHair, GridDensity::kCoarse, GridDensity::kFine},
             {GridType::kNone, GridDensity::kMedium, GridDensity::kMedium}}) {
        view.gridType = type;
        view.gridTime = time;
        view.gridLevel = level;
        for (const double ratio : {1.0, 2.0}) {
            for (const auto& [beats, noTempo] :
                 std::vector<std::pair<bool, bool>>{{false, false}, {true, false}, {true, true}}) {
                // The page sends Cross hair in Trig only.
                if (beats && type == GridType::kCrossHair) {
                    continue;
                }
                FrameView frame;
                std::vector<float> values(std::size_t{2} * 10, 0.0f);
                frame.mode = protocol::FrameMode::kMinMax;
                frame.pointCount = 10;
                frame.views = {{.channelView = 0, .values = values},
                               {.channelView = 1, .values = values}};
                if (!beats) {
                    // Trig: the trigger point where a new instance's Position puts it.
                    frame.triggerPosition = static_cast<float>(protocol::kDefaultTriggerPosition);
                }
                if (noTempo) {
                    // As the engine sends it: no cycle fields.
                    frame.flags = static_cast<std::uint8_t>(
                        static_cast<unsigned>(protocol::FrameFlag::kSynced) |
                        static_cast<unsigned>(protocol::FrameFlag::kNoTempo));
                } else if (beats) {
                    frame.flags = static_cast<std::uint8_t>(protocol::FrameFlag::kSynced);
                    frame.cycleStart = 4.0f;
                    frame.spanQuarterNotes = 8.0f;
                    frame.barQuarterNotes = 4.0f;
                }
                view.viewColours = {};
                const auto width = static_cast<std::size_t>(300 * ratio);
                const auto height = static_cast<std::size_t>(200 * ratio);
                const auto drawn = draw(width, height, &frame, view, ratio, true);
                ASSERT_TRUE(drawn);
                std::vector<VerticalLine> musical = musicalGridLines(
                    4.0, 8.0, 4.0, view.view,
                    {.time = time, .widthCss = static_cast<double>(width) / ratio});
                const GridLines lines =
                    gridLines(static_cast<double>(width), static_cast<double>(height), 2, ratio,
                              {.type = type,
                               .level = level,
                               .columns = gridColumns(time),
                               .vertical = beats && !noTempo ? &musical : nullptr,
                               .trigger = protocol::kDefaultTriggerPosition});
                ASSERT_EQ(lines.vertical.empty(), type == GridType::kNone);
                ASSERT_EQ(lines.ticks.empty(), type != GridType::kCrossHair);
                // A line's middle column or row is wholly covered: the line's colour there.
                const std::size_t row = height / 2 + 3;
                for (const GridLines::Vertical& line : lines.vertical) {
                    const auto x = static_cast<std::size_t>(std::floor(line.x));
                    const std::uint8_t* pixel = drawn->at(x, row);
                    EXPECT_EQ(pixel[1], line.strong ? 0 : 255) << line.x << " at " << ratio;
                    EXPECT_EQ(pixel[2], line.strong ? 255 : 0) << line.x << " at " << ratio;
                }
                const std::size_t column = 7;
                for (const GridLines::Horizontal& line : lines.horizontal) {
                    // The bottom lane's lowest level line falls just below the screen, as on the
                    // page.
                    if (line.y >= static_cast<double>(height)) {
                        continue;
                    }
                    // From the top on the page; the picture's rows count from the bottom.
                    const auto y = height - 1 - static_cast<std::size_t>(std::floor(line.y));
                    const std::uint8_t* pixel = drawn->at(column, y);
                    EXPECT_EQ(pixel[1], line.axis ? 0 : 255) << line.y << " at " << ratio;
                    EXPECT_EQ(pixel[2], line.axis ? 255 : 0) << line.y << " at " << ratio;
                }
                // Grid's divisions that None and Cross hair leave out: the screen there.
                const GridLines full =
                    gridLines(static_cast<double>(width), static_cast<double>(height), 2, ratio,
                              {.columns = gridColumns(time),
                               .vertical = beats && !noTempo ? &musical : nullptr});
                for (const GridLines::Vertical& line : full.vertical) {
                    if (std::ranges::any_of(lines.vertical, [&](const GridLines::Vertical& kept) {
                            return std::abs(kept.x - line.x) < 2.0 * ratio;
                        })) {
                        continue;
                    }
                    const std::uint8_t* pixel =
                        drawn->at(static_cast<std::size_t>(std::floor(line.x)), row);
                    EXPECT_EQ(pixel[1], 0) << "no line at " << line.x << " at " << ratio;
                    EXPECT_EQ(pixel[2], 0) << "no line at " << line.x << " at " << ratio;
                }
                // A tick's pixel a step in from its end, clear of the line it marks.
                for (const GridLines::Tick& tick : lines.ticks) {
                    // As the lowest level line, its tick may fall just below the screen.
                    if (tick.y0 + 1.0 >= static_cast<double>(height)) {
                        continue;
                    }
                    const bool vertical = tick.x0 == tick.x1;
                    const auto x =
                        static_cast<std::size_t>(std::floor(tick.x0)) + (vertical ? 0 : 1);
                    const auto y =
                        height - 1 -
                        (static_cast<std::size_t>(std::floor(tick.y0)) + (vertical ? 1 : 0));
                    const std::uint8_t* pixel = drawn->at(x, y);
                    EXPECT_EQ(pixel[1], tick.strong ? 0 : 255) << tick.x0 << ", " << tick.y0;
                    EXPECT_EQ(pixel[2], tick.strong ? 255 : 0) << tick.x0 << ", " << tick.y0;
                }
                // Between lines, the screen.
                const std::uint8_t* clear = drawn->at(3, height / 2 + 3);
                EXPECT_EQ(clear[1], 0);
                EXPECT_EQ(clear[2], 0);
                EXPECT_EQ(clear[3], 255);
            }
        }
    }
}

// Beats' Fine step guards its finer lines in CSS pixels, as the page measures its screen: at
// ratio 2, two bars on a screen 200 CSS pixels wide put sixteenths 6.25 CSS pixels apart (12.5
// device pixels), closer than the guard, so the editor draws Medium's beats with strong bars, as
// the page does there; guarding on device pixels would draw the sixteenths.
TEST_F(MetalTraceRendererTest, GuardsBeatsFineStepInCssPixels)
{
    TraceView view = scenes().front().view;
    view.screen = {0.0f, 0.0f, 0.0f, 1.0f};
    view.grid = {0.0f, 1.0f, 0.0f, 1.0f};
    view.axis = {0.0f, 0.0f, 1.0f, 1.0f};
    view.layout = protocol::ViewLayout::kStacked;
    view.gridTime = protocol::GridDensity::kFine;
    view.viewColours = {};
    constexpr double kRatio = 2.0;
    constexpr double kWidthCss = 200.0;
    const auto width = static_cast<std::size_t>(kWidthCss * kRatio);
    const auto height = static_cast<std::size_t>(150 * kRatio);
    const double sixteenthCss = 0.25 / 8.0 * kWidthCss;
    ASSERT_LT(sixteenthCss, protocol::kGridFineBeatLineGap);
    ASSERT_GE(sixteenthCss * kRatio, protocol::kGridFineBeatLineGap);

    FrameView frame;
    std::vector<float> values(std::size_t{2} * 10, 0.0f);
    frame.mode = protocol::FrameMode::kMinMax;
    frame.pointCount = 10;
    frame.views = {{.channelView = 0, .values = values}, {.channelView = 1, .values = values}};
    frame.flags = static_cast<std::uint8_t>(protocol::FrameFlag::kSynced);
    frame.cycleStart = 4.0f;
    frame.spanQuarterNotes = 8.0f;
    frame.barQuarterNotes = 4.0f;
    const auto drawn = draw(width, height, &frame, view, kRatio, true);
    ASSERT_TRUE(drawn);
    const std::vector<VerticalLine> musical =
        musicalGridLines(4.0, 8.0, 4.0, view.view, {.time = view.gridTime, .widthCss = kWidthCss});
    // Medium's beats: seven inside two bars, the bar line between them strong.
    ASSERT_EQ(musical.size(), 7U);
    const GridLines lines = gridLines(static_cast<double>(width), static_cast<double>(height), 2,
                                      kRatio, {.vertical = &musical});
    const std::size_t row = height / 2 + 3;
    for (const GridLines::Vertical& line : lines.vertical) {
        const std::uint8_t* pixel = drawn->at(static_cast<std::size_t>(std::floor(line.x)), row);
        EXPECT_EQ(pixel[1], line.strong ? 0 : 255) << line.x;
        EXPECT_EQ(pixel[2], line.strong ? 255 : 0) << line.x;
    }
    // Where a sixteenth would be, between two beats, the screen.
    const auto sixteenth = static_cast<std::size_t>(std::floor(static_cast<double>(width) / 32.0));
    EXPECT_EQ(drawn->at(sixteenth, row)[1], 0);
    EXPECT_EQ(drawn->at(sixteenth, row)[2], 0);
}

// The grid's lanes are the page's: the frame's lanes, else its views (a mono track's one view,
// whatever was asked for), and for a frame with neither (No tempo, which lasts until the host
// plays) or before any frame, the lanes the page lays out from the views shown. Each case's page
// count differs from the frame's own, so taking the wrong one draws level lines the page's labels
// do not match.
TEST_F(MetalTraceRendererTest, LaysTheGridOutInThePagesLanes)
{
    struct Case
    {
        const char* name;
        std::size_t views;
        std::size_t lanes;
        bool noFrame;
        protocol::ViewLayout layout;
        std::size_t emptyFrameLanes;
        std::size_t expected;
    };
    constexpr auto kStacked = protocol::ViewLayout::kStacked;
    const std::vector<Case> cases = {
        {.name = "No tempo, stacked",
         .views = 0,
         .lanes = 0,
         .noFrame = false,
         .layout = kStacked,
         .emptyFrameLanes = 3,
         .expected = 3},
        {.name = "before any frame, stacked",
         .views = 0,
         .lanes = 0,
         .noFrame = true,
         .layout = kStacked,
         .emptyFrameLanes = 2,
         .expected = 2},
        {.name = "No tempo, overlaid",
         .views = 0,
         .lanes = 0,
         .noFrame = false,
         .layout = protocol::ViewLayout::kOverlaid,
         .emptyFrameLanes = 1,
         .expected = 1},
        {.name = "three lanes, overlaid",
         .views = 0,
         .lanes = 3,
         .noFrame = false,
         .layout = protocol::ViewLayout::kOverlaid,
         .emptyFrameLanes = 3,
         .expected = 1},
        {.name = "a mono track's one view",
         .views = 1,
         .lanes = 0,
         .noFrame = false,
         .layout = kStacked,
         .emptyFrameLanes = 2,
         .expected = 1},
        {.name = "two views",
         .views = 2,
         .lanes = 0,
         .noFrame = false,
         .layout = kStacked,
         .emptyFrameLanes = 4,
         .expected = 2},
        {.name = "three lanes",
         .views = 0,
         .lanes = 3,
         .noFrame = false,
         .layout = kStacked,
         .emptyFrameLanes = 1,
         .expected = 3},
    };
    TraceView view = scenes().front().view;
    view.screen = {0.0f, 0.0f, 0.0f, 1.0f};
    view.grid = {0.0f, 1.0f, 0.0f, 1.0f};
    view.axis = {0.0f, 0.0f, 1.0f, 1.0f};
    // The traces themselves invisible: the grid alone.
    view.viewColours = {};
    view.laneSets = {};
    view.fallbackColours = {{0.0f, 0.0f, 0.0f, 0.0f}};
    const std::vector<float> values(std::size_t{2} * 10, 0.0f);
    for (const Case& each : cases) {
        FrameView frame;
        frame.mode = protocol::FrameMode::kMinMax;
        frame.pointCount = 10;
        if (each.views == 0 && each.lanes == 0) {
            frame.flags =
                static_cast<std::uint8_t>(static_cast<unsigned>(protocol::FrameFlag::kSynced) |
                                          static_cast<unsigned>(protocol::FrameFlag::kNoTempo));
        }
        for (std::size_t index = 0; index < each.views; ++index) {
            frame.views.push_back(
                {.channelView = static_cast<std::uint8_t>(index), .values = values});
        }
        for (std::size_t index = 0; index < each.lanes; ++index) {
            frame.lanes.emplace_back(values);
        }
        view.layout = each.layout;
        view.emptyFrameLanes = each.emptyFrameLanes;
        constexpr std::size_t kWidth = 300;
        constexpr std::size_t kHeight = 240;
        const auto drawn = draw(kWidth, kHeight, each.noFrame ? nullptr : &frame, view, 1.0, true);
        ASSERT_TRUE(drawn) << each.name;
        const GridLines lines = gridLines(static_cast<double>(kWidth), static_cast<double>(kHeight),
                                          each.expected, 1.0, {});
        const std::size_t column = 7;
        for (const GridLines::Horizontal& line : lines.horizontal) {
            if (line.y >= static_cast<double>(kHeight)) {
                continue;
            }
            const auto y = kHeight - 1 - static_cast<std::size_t>(std::floor(line.y));
            const std::uint8_t* pixel = drawn->at(column, y);
            EXPECT_EQ(pixel[1], line.axis ? 0 : 255) << each.name << ", line at " << line.y;
            EXPECT_EQ(pixel[2], line.axis ? 255 : 0) << each.name << ", line at " << line.y;
        }
    }
}

// Metal refusing the memory for the coverage targets is reported, so the editor can say so
// (nativeGraphicsLost), and nothing is drawn; once it grants it again, drawing resumes.
TEST_F(MetalTraceRendererTest, ReportsRefusedCoverageTargets)
{
    const std::vector<Scene> all = scenes();
    renderer->refuseCoverageTargetsForTesting(true);
    EXPECT_FALSE(draw(all.front()));
    renderer->refuseCoverageTargetsForTesting(false);
    const auto drawn = draw(all.front());
    ASSERT_TRUE(drawn);
    expectSamePicture(all.front(), *drawn);
}

// The frame barcode is drawn on every picture the switch is on for, whatever the renderer takes
// from the frame: before any frame, the grid alone, and each fixture scene's frame (playhead or
// none, lanes or views, Compare, nothing to draw). Each strip reads back as the values drawn.
TEST_F(MetalTraceRendererTest, DrawsTheFrameBarcodeOnEveryPicture)
{
    const auto expectStrip = [](const Picture& picture, double ratio, const BarcodeValues& values,
                                const std::string& name) {
        const BarcodeStrip strip = barcodeStrip(static_cast<double>(picture.width),
                                                static_cast<double>(picture.height), ratio);
        const std::array rows{barcodeRow(values.picture), barcodeRow(values.drawnFor)};
        for (std::size_t row = 0; row < rows.size(); ++row) {
            for (std::size_t cell = 0; cell < kBarcodeRowCells; ++cell) {
                const auto x = static_cast<std::size_t>(
                    strip.left + (static_cast<double>(cell) + 0.5) * strip.cell);
                const auto fromTop = static_cast<std::size_t>(
                    strip.top + (static_cast<double>(row) + 0.5) * strip.cell);
                const std::uint8_t* pixel = picture.at(x, picture.height - 1 - fromTop);
                EXPECT_EQ(pixel[0], rows[row][cell] ? 255 : 0)
                    << name << ", row " << row << ", cell " << cell;
                EXPECT_EQ(pixel[3], 255) << name;
            }
        }
    };
    const std::vector<Scene> all = scenes();
    BarcodeValues values{.picture = 41, .drawnFor = 60123};
    for (const double ratio : {1.0, 2.0}) {
        const auto width = static_cast<std::size_t>(400 * ratio);
        const auto height = static_cast<std::size_t>(200 * ratio);
        const auto bare = draw(width, height, nullptr, all.front().view, ratio, true, &values);
        ASSERT_TRUE(bare);
        expectStrip(*bare, ratio, values, "no frame");
    }
    for (const Scene& scene : all) {
        ++values.picture;
        const FrameView frame = scene.frame();
        const auto drawn =
            draw(scene.width, scene.height, &frame, scene.view, scene.ratio, false, &values);
        ASSERT_TRUE(drawn) << scene.name;
        expectStrip(*drawn, scene.ratio, values, scene.name);
    }
}

// Metal refusing the memory for a frame's buffers is reported as well, and costs the renderer
// nothing: each refused frame gives back the buffer slot it waited for, so after more refusals
// than there are slots the next frame is still drawn rather than waiting forever for one.
TEST_F(MetalTraceRendererTest, ReportsRefusedFrameBuffers)
{
    const std::vector<Scene> all = scenes();
    ASSERT_TRUE(draw(all.front()));
    const std::size_t free = renderer->freeSlotsForTesting();
    renderer->refuseFrameBuffersForTesting(true);
    // The next slot was never allocated: the refusal lands there.
    EXPECT_FALSE(draw(all.front()));
    ASSERT_EQ(renderer->freeSlotsForTesting(), free) << "the refused frame gave its slot back";
    for (int attempt = 0; attempt < 8; ++attempt) {
        EXPECT_FALSE(draw(all.front())) << "attempt " << attempt;
    }
    renderer->refuseFrameBuffersForTesting(false);
    const auto drawn = draw(all.front());
    ASSERT_TRUE(drawn);
    expectSamePicture(all.front(), *drawn);
}

} // namespace
} // namespace orizon::editor
