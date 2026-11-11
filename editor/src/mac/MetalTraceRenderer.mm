#include "MetalTraceRenderer.h"

#include "MetalHandoff.h"
#include "editor/TraceGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace orizon::editor {

namespace {

// Each trace's coverage goes to one channel of an offscreen target: four channels a target, and
// four targets, so a frame's traces (at most kMaxLanes) are layered in one pass
// (trace-geometry.ts, coverageAttachments and tracesPerPass).
constexpr std::size_t kCoverageTargets = 4;
constexpr std::size_t kTracesPerPass = kCoverageTargets * 4;
static_assert(protocol::kMaxLanes <= kTracesPerPass);
// Frames the GPU may still be reading while the next ones are encoded (Metal's triple buffering).
constexpr std::size_t kSlots = 3;
// The playhead's edges fade over this many device pixels: a hard line one pixel wide (a 1x display)
// alternates between one bright pixel and two dim ones as it crosses pixels, which flashes as it
// moves; a soft one looks much the same wherever it sits. On Retina, where the line is two pixels
// wide, the fade softens it by less than a point.
constexpr float kPlayheadSoftness = 1.5f;
constexpr auto kDimmedAlpha = static_cast<float>(protocol::kWaitingAlpha);
constexpr auto kPreviousCycleAlpha = static_cast<float>(protocol::kPreviousCycleAlpha);

// The trace pass's uniforms, laid out as the shaders' TraceUniforms: scalars only, so both sides
// agree without alignment rules.
struct TraceUniforms
{
    std::int32_t count = 0;
    std::int32_t stride = 1;
    float offset = 0.0f;
    float positions = 1.0f;
    float frameStart = 0.0f;
    float frameWidth = 1.0f;
    float viewStart = 0.0f;
    float viewWidth = 1.0f;
    float center = 0.0f;
    float halfHeight = 1.0f;
    float scale = 1.0f;
    float lineHalf = 0.5f;
    float canvasX = 1.0f;
    float canvasY = 1.0f;
    std::uint32_t first = 0;
};

// The previous cycle right of a Beats playhead, laid out as the shaders' Fade: what is drawn from
// the audio (the traces and the shading) has its colour moved `keep` of the way from `screen`'s
// between `from` and `to`, in device pixels from the left, a pixel partly inside in proportion,
// and keeps its coverage, so it covers the grid as live traces do. keep 1 dims nothing. Its float4
// aligns it to 16 bytes in the shaders (Metal Shading Language Specification, Table 2.3), so it is
// here too.
struct alignas(16) Fade
{
    std::array<float, 4> screen{};
    float from = 0.0f;
    float to = 0.0f;
    float keep = 1.0f;
};
static_assert(sizeof(Fade) == 32, "laid out as the shaders' Fade");

// The composite pass's uniforms, laid out as the shaders' CompositeUniforms. Their float4 colours
// align the shaders' struct to 16 bytes (Metal Shading Language Specification, Table 2.3), which
// puts the fade at 272 and rounds the size up to 304: bound with fewer bytes, every composite draw
// is a binding shorter than the shader's argument, which Metal's API validation stops on.
struct alignas(16) CompositeUniforms
{
    std::array<std::array<float, 4>, kTracesPerPass> colours{};
    std::int32_t traces = 0;
    std::int32_t attachments = 0;
    Fade fade;
};
static_assert(sizeof(CompositeUniforms) % 16 == 0, "rounded up as the shaders' CompositeUniforms");
static_assert(offsetof(CompositeUniforms, fade) == 272, "the fade where the shaders read it");

// A rectangle in device pixels from the bottom left, in one colour (not premultiplied). A soft
// axis has its edges at their exact positions with coverage; a hard one is left to the
// rasterizer, as the page's triangles are.
struct BandInstance
{
    float left = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float top = 0.0f;
    std::array<float, 4> colour{};
    std::int32_t softX = 0;
    std::int32_t softY = 0;
    // Partly covered pixels give off the share of light they cover, not of sRGB value (below).
    std::int32_t linearCoverage = 0;
    // Device pixels the soft x edges fade over (0: a box, each pixel covered by its overlap).
    float softness = 0.0f;
};
static_assert(sizeof(BandInstance) == 48, "laid out as the shaders' Band");

// The page's shaders (ui/src/render/trace-renderer.ts) in Metal Shading Language, line for line,
// with the band shader for what the page draws as triangles or on its grid canvas. Metal's
// fragment positions count down from the top: the shaders turn them into the page's, from the
// bottom left, so the arithmetic is the page's.
std::string shaderSource()
{
    std::string layering;
    for (std::size_t index = 0; index < kTracesPerPass; ++index) {
        const std::string k = std::to_string(index);
        const std::string target = std::to_string(index / 4);
        const char channel = "rgba"[index % 4];
        // Each step with its own fixed indices: an array indexed at run time is kept in slow
        // memory (trace-renderer.ts, compositeLayering).
        layering.append("  if (").append(k).append(" < u.traces) {\n    float a = u.colours[");
        layering.append(k).append("].a * coverage").append(target).append(".");
        layering.push_back(channel);
        layering.append(";\n    rgb = rgb * (1.0 - a) + u.colours[").append(k);
        layering.append("].rgb * a;\n    alpha = alpha * (1.0 - a) + a;\n  }\n");
    }
    return R"(#include <metal_stdlib>
using namespace metal;

struct TraceUniforms {
  int count;
  int stride;
  float offset;
  float positions;
  float frameStart;
  float frameWidth;
  float viewStart;
  float viewWidth;
  float center;
  float halfHeight;
  float scale;
  float lineHalf;
  float canvasX;
  float canvasY;
  uint first;
};

float pixelX(constant TraceUniforms& u, float index) {
  return (u.frameStart + index / u.positions * u.frameWidth - u.viewStart) / u.viewWidth * u.canvasX;
}
float pixelY(constant TraceUniforms& u, float value) {
  float halfPixels = u.halfHeight * 0.5 * u.canvasY;
  float limit = u.scale * 1e15 / halfPixels;
  return (u.center + 1.0) * 0.5 * u.canvasY + clamp(value, -limit, limit) / u.scale * halfPixels;
}
float2 clipped(float2 a, float2 b, float bottom, float top) {
  if (a.y > top) {
    return float2(a.x + (top - a.y) / (b.y - a.y) * (b.x - a.x), top);
  }
  if (a.y < bottom) {
    return float2(a.x + (bottom - a.y) / (b.y - a.y) * (b.x - a.x), bottom);
  }
  return a;
}

bool point(constant TraceUniforms& u, device const uint* values, int index, thread float& lo,
           thread float& hi) {
  lo = 0.0;
  hi = 0.0;
  if (index < 0 || index >= u.count) {
    return false;
  }
  uint at = u.first + uint(index * u.stride);
  uint low = values[at];
  uint high = u.stride == 2 ? values[at + 1] : low;
  if ((low & 0x7f800000u) == 0x7f800000u || (high & 0x7f800000u) == 0x7f800000u) {
    return false;
  }
  lo = pixelY(u, as_type<float>(low));
  hi = pixelY(u, as_type<float>(high));
  return true;
}

struct TracePiece {
  float4 position [[position]];
  float2 here [[flat]];
  float2 next [[flat]];
  int part [[flat]];
  float4 line [[flat]];
};

vertex TracePiece traceVertex(uint vertexId [[vertex_id]], uint instance [[instance_id]],
                              constant TraceUniforms& u [[buffer(0)]],
                              device const uint* values [[buffer(1)]]) {
  TracePiece out;
  out.part = int(vertexId / 6);
  int corner = int(vertexId % 6);
  bool far = corner == 2 || corner == 3 || corner == 5;
  bool upper = corner == 1 || corner == 4 || corner == 5;
  int i = int(instance);
  float pad = u.lineHalf + 0.75;
  float lo;
  float hi;
  bool present = point(u, values, i, lo, hi);
  float lo2;
  float hi2;
  bool joined = point(u, values, i + 1, lo2, hi2);
  float loBefore;
  float hiBefore;
  bool before = point(u, values, i - 1, loBefore, hiBefore);
  out.here = float2(lo, hi);
  out.next = float2(lo2, hi2);
  out.line = float4(0.0);
  float xa = pixelX(u, float(i) + u.offset);
  float xb = joined ? pixelX(u, float(i + 1) + u.offset) : xa;
  float2 c = float2(0.0);
  if (out.part == 0 && present && joined) {
    float low = clamp(min(lo, lo2), 0.0, u.canvasY);
    float high = clamp(max(hi, hi2), 0.0, u.canvasY);
    c = float2(far ? xb + 0.5 : xa - 0.5, upper ? high : low);
    out.line = float4(xa, xb, 0.0, 0.0);
  } else if ((out.part == 1 || out.part == 2) && present && joined) {
    float bottom = -4.0 * u.canvasY;
    float top = 5.0 * u.canvasY;
    float2 a = float2(xa, out.part == 1 ? hi : lo);
    float2 b = float2(xb, out.part == 1 ? hi2 : lo2);
    if (!((a.y > top && b.y > top) || (a.y < bottom && b.y < bottom))) {
      float2 a2 = clipped(a, b, bottom, top);
      float2 b2 = clipped(b, a, bottom, top);
      float2 along = b2 - a2;
      float run = length(along);
      float2 direction = run > 0.0 ? along / run : float2(1.0, 0.0);
      float2 across = float2(-direction.y, direction.x);
      c = (far ? b2 + direction * pad : a2 - direction * pad) + (upper ? across : -across) * pad;
      out.line = float4(a2, b2);
    }
  } else if (out.part == 3 && present && !(before && joined)) {
    float low = clamp(lo - pad, 0.0, u.canvasY);
    float high = clamp(hi + pad, 0.0, u.canvasY);
    c = float2(far ? xa + pad : xa - pad, upper ? high : low);
    out.line = float4(xa, 0.0, 0.0, 0.0);
  }
  out.position = float4(c.x / u.canvasX * 2.0 - 1.0, c.y / u.canvasY * 2.0 - 1.0, 0.0, 1.0);
  return out;
}

float segment(float2 p, float2 a, float2 b) {
  float2 ab = b - a;
  float dd = dot(ab, ab);
  float t = dd > 0.0 ? clamp(dot(p - a, ab) / dd, 0.0, 1.0) : 0.0;
  return length(p - a - t * ab);
}

fragment float4 traceFragment(TracePiece in [[stage_in]], constant TraceUniforms& u [[buffer(0)]]) {
  float2 p = float2(in.position.x, u.canvasY - in.position.y);
  float d;
  if (in.part == 0) {
    float f = (p.x - in.line.x) / (in.line.y - in.line.x);
    if (!(f >= 0.0 && f <= 1.0 && p.y >= mix(in.here.x, in.next.x, f) &&
          p.y <= mix(in.here.y, in.next.y, f))) {
      discard_fragment();
    }
    d = 0.0;
  } else if (in.part == 3) {
    d = length(float2(p.x - in.line.x, p.y - clamp(p.y, in.here.x, in.here.y)));
  } else {
    d = segment(p, in.line.xy, in.line.zw);
  }
  float covered = clamp(u.lineHalf + 0.5 - d, 0.0, 1.0);
  if (covered <= 0.0) {
    discard_fragment();
  }
  return float4(covered);
}

struct Corner {
  float4 position [[position]];
};

vertex Corner compositeVertex(uint vertexId [[vertex_id]]) {
  Corner out;
  out.position = float4(vertexId == 1 ? 3.0 : -1.0, vertexId == 2 ? 3.0 : -1.0, 0.0, 1.0);
  return out;
}

struct Fade {
  float4 screen;
  float from;
  float to;
  float keep;
};

// A colour in pixel column `pixel` under the fade, moved toward the screen's by the share of the
// pixel [pixel, pixel + 1) the faded span covers, as the page's dimming covers whole columns.
float3 dimmed(float3 colour, float pixel, Fade fade) {
  float covered = clamp(min(pixel + 1.0, fade.to) - max(pixel, fade.from), 0.0, 1.0);
  float keep = 1.0 - (1.0 - fade.keep) * covered;
  return fade.screen.rgb + keep * (colour - fade.screen.rgb);
}

struct CompositeUniforms {
  float4 colours[16];
  int traces;
  int attachments;
  Fade fade;
};

fragment float4 compositeFragment(Corner in [[stage_in]],
                                  constant CompositeUniforms& u [[buffer(0)]],
                                  texture2d<float, access::read> target0 [[texture(0)]],
                                  texture2d<float, access::read> target1 [[texture(1)]],
                                  texture2d<float, access::read> target2 [[texture(2)]],
                                  texture2d<float, access::read> target3 [[texture(3)]]) {
  uint2 at = uint2(in.position.xy);
  float4 coverage0 = target0.read(at);
  float4 coverage1 = u.attachments > 1 ? target1.read(at) : float4(0.0);
  float4 coverage2 = u.attachments > 2 ? target2.read(at) : float4(0.0);
  float4 coverage3 = u.attachments > 3 ? target3.read(at) : float4(0.0);
  float3 rgb = float3(0.0);
  float alpha = 0.0;
)" + layering +
           R"(  if (alpha <= 0.0) {
    discard_fragment();
  }
  // The traces dim as one group, as a layer with an opacity is composited.
  return float4(dimmed(rgb / alpha, floor(in.position.x), u.fade), alpha);
}

struct Band {
  float left;
  float right;
  float bottom;
  float top;
  float4 colour;
  int softX;
  int softY;
  int linearCoverage;
  float softness;
};

struct BandPixel {
  float4 position [[position]];
  uint band [[flat]];
};

// Six corners per band: two triangles (0, 1, 2) and (2, 1, 3), reaching a pixel past a soft edge.
vertex BandPixel bandVertex(uint vertexId [[vertex_id]], uint instance [[instance_id]],
                            device const Band* bands [[buffer(0)]],
                            constant float2& canvas [[buffer(1)]]) {
  Band band = bands[instance];
  int corner = int(vertexId % 6);
  bool far = corner == 2 || corner == 3 || corner == 5;
  bool upper = corner == 1 || corner == 4 || corner == 5;
  float reachX = band.softX != 0 ? 1.0 + band.softness : 0.0;
  float reachY = band.softY != 0 ? 1.0 : 0.0;
  float2 c = float2(far ? band.right + reachX : band.left - reachX,
                    upper ? band.top + reachY : band.bottom - reachY);
  BandPixel out;
  out.position = float4(c / canvas * 2.0 - 1.0, 0.0, 1.0);
  out.band = instance;
  return out;
}

// sRGB's encoding of a linear value (IEC 61966-2-1).
float srgbEncoded(float linear) {
  return linear <= 0.0031308 ? 12.92 * linear : 1.055 * pow(linear, 1.0 / 2.4) - 0.055;
}

// How much of the pixel [pixel, pixel + 1) lies between low and high (TraceGeometry's
// bandCoverage).
float covers(float pixel, float low, float high) {
  return clamp(min(pixel + 1.0, high) - max(pixel, low), 0.0, 1.0);
}

fragment float4 bandFragment(BandPixel in [[stage_in]], device const Band* bands [[buffer(0)]],
                             constant float2& canvas [[buffer(1)]],
                             constant Fade& fade [[buffer(2)]]) {
  Band band = bands[in.band];
  float2 pixel = floor(float2(in.position.x, canvas.y - in.position.y));
  float coverage = 1.0;
  if (band.softX != 0) {
    if (band.softness > 0.0) {
      // The box blurred by a box `softness` wide: the overlap averaged over shifts across it
      // (exact enough at 16, the overlap being linear between its corners).
      float sum = 0.0;
      for (int step = 0; step < 16; ++step) {
        float shift = band.softness * ((float(step) + 0.5) / 16.0 - 0.5);
        sum += covers(pixel.x, band.left + shift, band.right + shift);
      }
      coverage *= sum / 16.0;
    } else {
      coverage *= covers(pixel.x, band.left, band.right);
    }
  }
  if (band.softY != 0) {
    coverage *= covers(pixel.y, band.bottom, band.top);
  }
  // Blending mixes sRGB values: a pixel half covered would give off about a fifth of the light,
  // so a line between two pixels would dim and one moving by fractions of a pixel would flash.
  // Over the dark screen, coverage encoded as sRGB gives off the light it covers.
  coverage = band.colour.a * (band.linearCoverage != 0 ? srgbEncoded(coverage) : coverage);
  if (coverage <= 0.0) {
    discard_fragment();
  }
  return float4(dimmed(band.colour.rgb, pixel.x, fade), coverage);
}
)";
}

// Not premultiplied colours blended as the page's canvas is: alpha accumulates as coverage.
void blendOver(MTLRenderPipelineColorAttachmentDescriptor* attachment)
{
    attachment.blendingEnabled = YES;
    attachment.rgbBlendOperation = MTLBlendOperationAdd;
    attachment.alphaBlendOperation = MTLBlendOperationAdd;
    attachment.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    attachment.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    attachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
    attachment.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
}

std::array<float, 4> dimmed(const Rgba& colour, bool dim)
{
    return {colour[0], colour[1], colour[2], dim ? colour[3] * kDimmedAlpha : colour[3]};
}

class Renderer final : public MetalTraceRenderer
{
public:
    Renderer(id<MTLDevice> device, id<MTLRenderPipelineState> composite,
             id<MTLRenderPipelineState> band, std::array<id<MTLRenderPipelineState>, 4> trace)
        : device(device), compositePipeline(composite), bandPipeline(band),
          tracePipelines(std::move(trace)), slotsFree(dispatch_semaphore_create(kSlots))
    {}

    ~Renderer() override
    {
        // Every slot back from the GPU before the semaphore goes: libdispatch refuses to free one
        // below its initial count.
        for (std::size_t slot = 0; slot < kSlots; ++slot) {
            dispatch_semaphore_wait(slotsFree, DISPATCH_TIME_FOREVER);
        }
        for (std::size_t slot = 0; slot < kSlots; ++slot) {
            dispatch_semaphore_signal(slotsFree);
        }
    }

    void refuseCoverageTargetsForTesting(bool refuse) override { refuseTargets = refuse; }
    void refuseFrameBuffersForTesting(bool refuse) override { refuseBuffers = refuse; }
    std::size_t freeSlotsForTesting() override
    {
        std::size_t free = 0;
        while (free < kSlots &&
               dispatch_semaphore_wait(
                   slotsFree, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC)) == 0) {
            ++free;
        }
        for (std::size_t index = 0; index < free; ++index) {
            dispatch_semaphore_signal(slotsFree);
        }
        return free;
    }

    Result encode(id<MTLCommandBuffer> buffer, id<MTLTexture> target, const FrameView* frame,
                  const TraceView& view, double pixelRatio, bool grid,
                  const BarcodeValues* barcode) override
    {
        const auto width = static_cast<double>(target.width);
        const auto height = static_cast<double>(target.height);
        if (!fitCoverage(target.width, target.height)) {
            return Result::kOutOfMemory;
        }

        // The traces: views in ChannelView order, or lanes, each in its own lane of the screen.
        std::vector<std::span<const float>> blocks;
        std::vector<Rgba> colours;
        if (frame != nullptr && !frame->lanes.empty()) {
            colours = view.laneColours(*frame);
            blocks = frame->lanes;
        } else if (frame != nullptr) {
            for (const FrameView::View& each : frame->views) {
                blocks.push_back(each.values);
                colours.push_back(view.viewColours.at(static_cast<std::size_t>(each.channelView)));
            }
        }
        if (blocks.size() > kTracesPerPass) {
            blocks.resize(kTracesPerPass);
        }

        // A tempo-synced sweep's playhead, as a fraction of the screen from the left, and the
        // previous cycle to its right, which starts under the line: its traces and shading dim, the
        // grid does not.
        std::optional<double> position;
        if (frame != nullptr && frame->has(protocol::FrameFlag::kSynced) && frame->cursor >= 0.0f) {
            position = (frame->cursor - view.view.start) / (view.view.end - view.view.start);
        }
        const std::optional<PixelBand> line =
            position ? std::optional(playheadBand(*position, width, pixelRatio)) : std::nullopt;
        const Fade undimmed{
            .screen = view.screen, .from = 0.0f, .to = static_cast<float>(width), .keep = 1.0f};
        Fade fade = undimmed;
        if (line && *position < 1.0) {
            fade.from = static_cast<float>(line->left);
            fade.keep = kPreviousCycleAlpha;
        }

        // What the page draws as triangles and on its grid canvas, in the order it draws it.
        std::vector<BandInstance> under;
        if (grid) {
            addGrid(under, frame, view, blocks.size(), width, height, pixelRatio);
        }
        const std::size_t gridBands = under.size();
        if (frame != nullptr) {
            const auto colour = dimmed(view.cancellation, view.dim);
            for (const Band& band :
                 cancellationBands(*frame, view.layout, view.comparedLanes(*frame), view.view)) {
                under.push_back({.left = clipToPixels(band.left, width),
                                 .right = clipToPixels(band.right, width),
                                 .bottom = clipToPixels(band.bottom, height),
                                 .top = clipToPixels(band.top, height),
                                 .colour = colour,
                                 .softX = 1});
            }
        }
        std::vector<BandInstance> over;
        if (line && *position >= 0.0 && *position <= 1.0) {
            over.push_back({.left = static_cast<float>(line->left),
                            .right = static_cast<float>(line->right),
                            .bottom = 0.0f,
                            .top = static_cast<float>(height),
                            .colour = view.cursor,
                            .softX = 1,
                            .linearCoverage = 1,
                            .softness = kPlayheadSoftness});
        }
        // The frame barcode over everything, on every picture, as the page draws it.
        if (barcode != nullptr) {
            addBarcode(over, *barcode, width, height, pixelRatio);
        }

        const bool traces = !blocks.empty() && frame->pointCount > 0;
        std::size_t valueCount = 0;
        if (traces) {
            for (const auto& block : blocks) {
                valueCount += block.size();
            }
        }
        // The next slot, once the GPU is done with it.
        dispatch_semaphore_wait(slotsFree, DISPATCH_TIME_FOREVER);
        Slot& slot = slots[nextSlot];
        if (!slot.reserve(device, valueCount * sizeof(float),
                          (under.size() + over.size()) * sizeof(BandInstance), refuseBuffers)) {
            dispatch_semaphore_signal(slotsFree);
            return Result::kOutOfMemory;
        }
        nextSlot = (nextSlot + 1) % kSlots;
        dispatch_semaphore_t freed = slotsFree;
        [buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
          handlerRuns(done);
          dispatch_semaphore_signal(freed);
        }];
        handlerAdded(buffer);
        std::vector<std::uint32_t> firsts;
        auto* values = static_cast<std::byte*>(slot.values.contents);
        std::size_t written = 0;
        if (traces) {
            for (const auto& block : blocks) {
                firsts.push_back(static_cast<std::uint32_t>(written));
                std::memcpy(values + written * sizeof(float), block.data(), block.size_bytes());
                written += block.size();
            }
        }
        auto* bands = static_cast<BandInstance*>(slot.bands.contents);
        std::ranges::copy(under, bands);
        std::ranges::copy(over, bands + under.size());

        CompositeUniforms composite;
        if (traces) {
            composite =
                encodeCoverage(buffer, slot, *frame, view, blocks, colours, firsts, width, height);
        }
        composite.fade = fade;

        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        const Rgba& screen = view.screen;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(
            screen[0] * screen[3], screen[1] * screen[3], screen[2] * screen[3], screen[3]);
        id<MTLRenderCommandEncoder> encoder = [buffer renderCommandEncoderWithDescriptor:pass];
        const std::array<float, 2> canvas{static_cast<float>(width), static_cast<float>(height)};
        const auto drawBands = [&](std::size_t first, std::size_t count, const Fade& bandFade) {
            if (count == 0) {
                return;
            }
            [encoder setRenderPipelineState:bandPipeline];
            [encoder setFragmentBytes:&bandFade length:sizeof(bandFade) atIndex:2];
            [encoder setVertexBuffer:slot.bands offset:first * sizeof(BandInstance) atIndex:0];
            [encoder setFragmentBuffer:slot.bands offset:first * sizeof(BandInstance) atIndex:0];
            [encoder setVertexBytes:canvas.data() length:sizeof(canvas) atIndex:1];
            [encoder setFragmentBytes:canvas.data() length:sizeof(canvas) atIndex:1];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:0
                        vertexCount:6
                      instanceCount:count];
        };
        drawBands(0, gridBands, undimmed);
        drawBands(gridBands, under.size() - gridBands, fade);
        if (traces) {
            // Then the traces layered over the screen, once.
            [encoder setRenderPipelineState:compositePipeline];
            [encoder setFragmentBytes:&composite length:sizeof(composite) atIndex:0];
            for (std::size_t index = 0; index < kCoverageTargets; ++index) {
                [encoder setFragmentTexture:coverage[index] atIndex:index];
            }
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        drawBands(under.size(), over.size(), undimmed);
        [encoder endEncoding];
        return Result::kDrawn;
    }

private:
    // One frame's buffers, kept while the GPU may read them, grown when a frame needs more.
    struct Slot
    {
        id<MTLBuffer> values;
        id<MTLBuffer> bands;

        bool reserve(id<MTLDevice> device, std::size_t valueBytes, std::size_t bandBytes,
                     bool refuse)
        {
            return grow(device, values, valueBytes, refuse) &&
                   grow(device, bands, bandBytes, refuse);
        }

        static bool grow(id<MTLDevice> device, id<MTLBuffer> __strong& buffer, std::size_t bytes,
                         bool refuse)
        {
            // At least something to bind, and room to spare so a frame a little larger does not
            // reallocate.
            const std::size_t wanted = std::max<std::size_t>(bytes, 256);
            if (buffer != nil && buffer.length >= wanted) {
                return true;
            }
            buffer = refuse ? nil
                            : [device newBufferWithLength:wanted + wanted / 2
                                                  options:MTLResourceStorageModeShared |
                                                          MTLResourceCPUCacheModeWriteCombined];
            return buffer != nil;
        }
    };

    static float clipToPixels(double clip, double size)
    {
        return static_cast<float>((clip + 1.0) / 2.0 * size);
    }

    // The coverage targets at the target's size, all four allocated together, as the page does.
    bool fitCoverage(NSUInteger width, NSUInteger height)
    {
        if (coverage[0] != nil && coverage[0].width == width && coverage[0].height == height) {
            return true;
        }
        MTLTextureDescriptor* descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:width
                                                              height:height
                                                           mipmapped:NO];
        descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        descriptor.storageMode = MTLStorageModePrivate;
        for (auto& texture : coverage) {
            texture = refuseTargets ? nil : [device newTextureWithDescriptor:descriptor];
            if (texture == nil) {
                coverage = {};
                return false;
            }
        }
        return true;
    }

    // Each trace's coverage into its channel of its target, the larger kept where its pieces
    // overlap; returns the composite's colours and counts.
    CompositeUniforms encodeCoverage(id<MTLCommandBuffer> buffer, const Slot& slot,
                                     const FrameView& frame, const TraceView& view,
                                     const std::vector<std::span<const float>>& blocks,
                                     const std::vector<Rgba>& colours,
                                     const std::vector<std::uint32_t>& firsts, double width,
                                     double height)
    {
        const bool minMax = frame.mode == protocol::FrameMode::kMinMax;
        TraceUniforms uniforms;
        uniforms.count = static_cast<std::int32_t>(frame.pointCount);
        uniforms.stride = minMax ? 2 : 1;
        uniforms.offset = minMax ? 0.5f : -frame.startFraction;
        uniforms.positions = minMax                        ? static_cast<float>(frame.pointCount)
                             : frame.visibleSamples > 0.0f ? frame.visibleSamples
                                                           : static_cast<float>(frame.pointCount);
        uniforms.frameStart = frame.viewStart;
        uniforms.frameWidth = frame.viewEnd - frame.viewStart;
        uniforms.viewStart = static_cast<float>(view.view.start);
        uniforms.viewWidth = static_cast<float>(view.view.end - view.view.start);
        uniforms.scale = static_cast<float>(std::pow(10.0, view.verticalScaleDb / 20.0));
        uniforms.lineHalf = static_cast<float>(view.lineWidth / 2.0);
        uniforms.canvasX = static_cast<float>(width);
        uniforms.canvasY = static_cast<float>(height);

        const std::vector<Lane> laneList = lanes(blocks.size(), view.layout);
        CompositeUniforms composite;
        composite.traces = static_cast<std::int32_t>(blocks.size());
        composite.attachments = static_cast<std::int32_t>((blocks.size() + 3) / 4);
        id<MTLRenderCommandEncoder> encoder = nil;
        std::size_t bound = kCoverageTargets;
        for (std::size_t index = 0; index < blocks.size(); ++index) {
            const std::size_t attachment = index / 4;
            if (attachment != bound) {
                [encoder endEncoding];
                MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = coverage[attachment];
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
                encoder = [buffer renderCommandEncoderWithDescriptor:pass];
                [encoder setVertexBuffer:slot.values offset:0 atIndex:1];
                bound = attachment;
            }
            [encoder setRenderPipelineState:tracePipelines[index % 4]];
            uniforms.center = static_cast<float>(laneList[index].center);
            uniforms.halfHeight = static_cast<float>(laneList[index].halfHeight);
            uniforms.first = firsts[index];
            [encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:0];
            [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
            // Four parts of six vertices each, one instance per point.
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                        vertexStart:0
                        vertexCount:24
                      instanceCount:frame.pointCount];
            const Rgba white{1.0f, 1.0f, 1.0f, 1.0f};
            composite.colours[index] =
                dimmed(index < colours.size() ? colours[index] : white, view.dim);
        }
        [encoder endEncoding];
        return composite;
    }

    // The strip's cells as opaque boxes, white or black (frame-barcode.ts).
    static void addBarcode(std::vector<BandInstance>& out, const BarcodeValues& values,
                           double width, double height, double pixelRatio)
    {
        const BarcodeStrip strip = barcodeStrip(width, height, pixelRatio);
        const std::array rows{barcodeRow(values.picture), barcodeRow(values.drawnFor)};
        for (std::size_t row = 0; row < rows.size(); ++row) {
            // From the top on the page; from the bottom here.
            const double top = height - (strip.top + static_cast<double>(row) * strip.cell);
            for (std::size_t cell = 0; cell < kBarcodeRowCells; ++cell) {
                const double left = strip.left + static_cast<double>(cell) * strip.cell;
                const float shade = rows[row][cell] ? 1.0f : 0.0f;
                out.push_back({.left = static_cast<float>(left),
                               .right = static_cast<float>(left + strip.cell),
                               .bottom = static_cast<float>(top - strip.cell),
                               .top = static_cast<float>(top),
                               .colour = {shade, shade, shade, 1.0f}});
            }
        }
    }

    // The page's grid (grid.ts), by the type and steps its trace view names: the Time step's
    // equal divisions, or a tempo-synced sweep's beats and bars in Beats mode (the frame's cycle
    // fields), and each lane's level and zero lines (the zero lines alone with None), as lines of
    // the page's width centred where it strokes them.
    static void addGrid(std::vector<BandInstance>& out, const FrameView* frame,
                        const TraceView& view, std::size_t blocks, double width, double height,
                        double pixelRatio)
    {
        // A frame with neither views nor lanes (No tempo) is laid out as the page lays it out.
        const std::size_t laneCount = view.layout != protocol::ViewLayout::kStacked ? 1
                                      : blocks > 0 ? blocks
                                                   : view.emptyFrameLanes;
        std::vector<VerticalLine> musical;
        const bool synced = frame != nullptr && frame->has(protocol::FrameFlag::kSynced) &&
                            !frame->has(protocol::FrameFlag::kNoTempo);
        if (synced) {
            // Fine's guard is in CSS pixels, as the page measures its screen.
            musical = musicalGridLines(frame->cycleStart, frame->spanQuarterNotes,
                                       frame->barQuarterNotes, view.view,
                                       {.time = view.gridTime,
                                        .division = view.beatDivision,
                                        .widthCss = width / std::max(1.0, pixelRatio)});
        }
        const GridLines lines = gridLines(width, height, laneCount, pixelRatio,
                                          {.type = view.gridType,
                                           .level = view.gridLevel,
                                           .columns = gridColumns(view.gridTime),
                                           .vertical = synced ? &musical : nullptr});
        const double half = lines.lineWidth / 2.0;
        for (const GridLines::Vertical& line : lines.vertical) {
            out.push_back({.left = static_cast<float>(line.x - half),
                           .right = static_cast<float>(line.x + half),
                           .bottom = 0.0f,
                           .top = static_cast<float>(height),
                           .colour = line.strong ? view.axis : view.grid,
                           .softX = 1});
        }
        for (const GridLines::Horizontal& line : lines.horizontal) {
            // From the top on the page's canvas; from the bottom here.
            const double y = height - line.y;
            out.push_back({.left = 0.0f,
                           .right = static_cast<float>(width),
                           .bottom = static_cast<float>(y - half),
                           .top = static_cast<float>(y + half),
                           .colour = line.axis ? view.axis : view.grid,
                           .softY = 1});
        }
    }

    id<MTLDevice> device;
    id<MTLRenderPipelineState> compositePipeline;
    id<MTLRenderPipelineState> bandPipeline;
    std::array<id<MTLRenderPipelineState>, 4> tracePipelines;
    std::array<id<MTLTexture>, kCoverageTargets> coverage;
    std::array<Slot, kSlots> slots;
    std::size_t nextSlot = 0;
    dispatch_semaphore_t slotsFree;
    bool refuseTargets = false;
    bool refuseBuffers = false;
};

} // namespace

std::unique_ptr<MetalTraceRenderer> MetalTraceRenderer::create(id<MTLDevice> device)
{
    if (device == nil) {
        return nullptr;
    }
    MTLCompileOptions* options = [MTLCompileOptions new];
    // The page's arithmetic exactly: no fast-math reassociation or flushed NaN tests.
    if (@available(macOS 15.0, *)) {
        options.mathMode = MTLMathModeSafe;
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        options.fastMathEnabled = NO;
#pragma clang diagnostic pop
    }
    NSError* error = nil;
    id<MTLLibrary> library =
        [device newLibraryWithSource:[NSString stringWithUTF8String:shaderSource().c_str()]
                             options:options
                               error:&error];
    if (library == nil) {
        NSLog(@"Sineless: the trace shaders did not build: %@", error);
        return nullptr;
    }
    const auto pipeline = [&](NSString* vertex, NSString* fragment, MTLPixelFormat format,
                              void (^configure)(MTLRenderPipelineColorAttachmentDescriptor*))
        -> id<MTLRenderPipelineState> {
        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [library newFunctionWithName:vertex];
        descriptor.fragmentFunction = [library newFunctionWithName:fragment];
        descriptor.colorAttachments[0].pixelFormat = format;
        configure(descriptor.colorAttachments[0]);
        NSError* failure = nil;
        id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:descriptor
                                                                                  error:&failure];
        if (state == nil) {
            NSLog(@"Sineless: a trace pipeline did not build: %@", failure);
        }
        return state;
    };
    id<MTLRenderPipelineState> composite =
        pipeline(@"compositeVertex", @"compositeFragment", MTLPixelFormatBGRA8Unorm,
                 ^(MTLRenderPipelineColorAttachmentDescriptor* attachment) {
                   blendOver(attachment);
                 });
    id<MTLRenderPipelineState> band =
        pipeline(@"bandVertex", @"bandFragment", MTLPixelFormatBGRA8Unorm,
                 ^(MTLRenderPipelineColorAttachmentDescriptor* attachment) {
                   blendOver(attachment);
                 });
    std::array<id<MTLRenderPipelineState>, 4> trace;
    const std::array<MTLColorWriteMask, 4> channels{MTLColorWriteMaskRed, MTLColorWriteMaskGreen,
                                                    MTLColorWriteMaskBlue, MTLColorWriteMaskAlpha};
    for (std::size_t channel = 0; channel < 4; ++channel) {
        const MTLColorWriteMask mask = channels[channel];
        trace[channel] = pipeline(@"traceVertex", @"traceFragment", MTLPixelFormatRGBA8Unorm,
                                  ^(MTLRenderPipelineColorAttachmentDescriptor* attachment) {
                                    // The larger kept, in this trace's channel only.
                                    attachment.blendingEnabled = YES;
                                    attachment.rgbBlendOperation = MTLBlendOperationMax;
                                    attachment.alphaBlendOperation = MTLBlendOperationMax;
                                    attachment.sourceRGBBlendFactor = MTLBlendFactorOne;
                                    attachment.destinationRGBBlendFactor = MTLBlendFactorOne;
                                    attachment.sourceAlphaBlendFactor = MTLBlendFactorOne;
                                    attachment.destinationAlphaBlendFactor = MTLBlendFactorOne;
                                    attachment.writeMask = mask;
                                  });
        if (trace[channel] == nil) {
            return nullptr;
        }
    }
    if (composite == nil || band == nil) {
        return nullptr;
    }
    return std::make_unique<Renderer>(device, composite, band, trace);
}

} // namespace orizon::editor
