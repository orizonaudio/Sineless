// Showcase video: the real editor page in a real WKWebView, fed by the real engines, filmed one
// video frame at a time. A song's stems play through one instance per track; a script clicks
// through the main features while a drawn cursor, camera moves and captions frame them; each
// frame is composed with Core Graphics and encoded with the song's mix as the soundtrack.
//
//   showcase --stems=DIR [--prefix=Promo] [--out=build/showcase/Sineless.mp4] [--frames=N]
//
// DIR holds the song exported from Live with "All Individual Tracks": "<prefix> <track>.wav" for
// each track and "<prefix>.wav", the mix; 16 bars at 120 BPM and 48 kHz, rendered as a loop.
//
// Time is stepped, not real: before each video frame the engines get exactly one frame of audio,
// and the frame is taken once the page has drawn it, so the video is smooth however slowly the
// frames are made. The window shows on screen while it runs (WebKit only draws visible pages);
// on a Retina display the editor is filmed at twice its size.

#include "editor/Editor.h"
#include "engine/Engine.h"
#include "engine/InstanceRegistry.h"
#include "engine/SettingJson.h"
#include "pluginterfaces/gui/iplugview.h"

#import <AVFoundation/AVFoundation.h>
#import <Cocoa/Cocoa.h>
#import <CoreText/CoreText.h>
#import <WebKit/WebKit.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace orizon;
using namespace Steinberg;

namespace {

constexpr double kRate = 48000.0;
constexpr double kTempo = 120.0;
constexpr int kFps = 60;
constexpr int kBlock = static_cast<int>(kRate) / kFps; // Audio per video frame.
constexpr double kPreroll = 2.0; // Seconds played before the first frame: one bar.
constexpr double kSeconds = 64.0;
// Where in the song the first frame falls: two bars before the loop's end, so the sweep rises
// under the title and the drop lands as the plugin appears.
constexpr double kSongStart = 26.0;
// The song starts at bar 17 of the set; the host's position counts from there.
constexpr double kSetStart = 32.0;

constexpr int kVideoWidth = 1920;
constexpr int kVideoHeight = 1080;
// The editor fills the frame: 16:9 in CSS pixels, at 1.5 video pixels each.
constexpr double kEditorWidth = 1280.0;
constexpr double kEditorHeight = 720.0;
constexpr double kPageScale = kVideoWidth / kEditorWidth;

struct Options
{
    std::string stems;
    std::string prefix = "Promo";
    std::string out = "build/showcase/Sineless.mp4";
    int frames = -1; // Stop early, for trying out the script.
};

Options parse(std::span<const char* const> arguments)
{
    Options options;
    for (const char* const text : arguments.subspan(1)) {
        const std::string argument = text;
        const auto equals = argument.find('=');
        const std::string key = argument.substr(0, equals);
        const std::string value = equals == std::string::npos ? "" : argument.substr(equals + 1);
        if (key == "--stems") {
            options.stems = value;
        } else if (key == "--prefix") {
            options.prefix = value;
        } else if (key == "--out") {
            options.out = value;
        } else if (key == "--frames") {
            options.frames = std::stoi(value);
        }
    }
    if (options.stems.empty()) {
        throw std::runtime_error("--stems=DIR is required: the song's tracks, exported from Live");
    }
    return options;
}

// ---------------------------------------------------------------------------------------------
// The song.

struct Stereo
{
    std::vector<float> left;
    std::vector<float> right;
    [[nodiscard]] std::size_t size() const { return left.size(); }
};

Stereo load(const std::string& path)
{
    NSError* error = nil;
    AVAudioFile* file = [[AVAudioFile alloc] initForReading:[NSURL fileURLWithPath:@(path.c_str())]
                                               commonFormat:AVAudioPCMFormatFloat32
                                                interleaved:NO
                                                      error:&error];
    if (file == nil) {
        throw std::runtime_error("cannot read " + path);
    }
    if (file.processingFormat.sampleRate != kRate) {
        throw std::runtime_error(path + " is not at 48 kHz");
    }
    const auto frames = static_cast<AVAudioFrameCount>(file.length);
    AVAudioPCMBuffer* buffer = [[AVAudioPCMBuffer alloc] initWithPCMFormat:file.processingFormat
                                                             frameCapacity:frames];
    if (![file readIntoBuffer:buffer error:&error]) {
        throw std::runtime_error("cannot read " + path);
    }
    const float* left = buffer.floatChannelData[0];
    const float* right = buffer.format.channelCount > 1 ? buffer.floatChannelData[1] : left;
    return {.left = {left, left + buffer.frameLength},
            .right = {right, right + buffer.frameLength}};
}

// ---------------------------------------------------------------------------------------------
// Easing and keyframes.

double clamp01(double x)
{
    return std::clamp(x, 0.0, 1.0);
}
double smooth(double x) // Ease in and out.
{
    x = clamp01(x);
    return x < 0.5 ? 4 * x * x * x : 1 - std::pow(-2 * x + 2, 3) / 2;
}
double easeOut(double x)
{
    return 1 - std::pow(1 - clamp01(x), 3);
}
double lerp(double a, double b, double x)
{
    return a + (b - a) * x;
}
// 0 before `from`, rising to 1 over `rise`, falling back to 0 over `fall` before `to`.
double shown(double t, double from, double to, double rise = 0.35, double fall = 0.35)
{
    return std::min(smooth((t - from) / rise), smooth((to - t) / fall));
}

struct Point
{
    double x = 0.0;
    double y = 0.0;
};

// A value that moves between targets over set times, eased.
template <typename T> class Track
{
public:
    explicit Track(T initial) : from(initial), to(initial) {}
    void move(double now, T target, double duration)
    {
        from = at(now);
        to = target;
        start = now;
        length = duration;
    }
    [[nodiscard]] T at(double t) const
    {
        const double x = length <= 0.0 ? 1.0 : smooth((t - start) / length);
        return mix(from, to, x);
    }
    [[nodiscard]] T target() const { return to; }

private:
    static double mix(double a, double b, double x) { return lerp(a, b, x); }
    static Point mix(Point a, Point b, double x)
    {
        return {.x = lerp(a.x, b.x, x), .y = lerp(a.y, b.y, x)};
    }
    T from;
    T to;
    double start = 0.0;
    double length = 0.0;
};

// ---------------------------------------------------------------------------------------------
// The page.

void waitUntil(const std::function<bool()>& done, double timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        // Window server events too, or WebKit counts the page as hidden and stops drawing it.
        @autoreleasepool {
            NSEvent* event =
                [NSApp nextEventMatchingMask:NSEventMaskAny
                                   untilDate:[NSDate dateWithTimeIntervalSinceNow:0.001]
                                      inMode:NSDefaultRunLoopMode
                                     dequeue:YES];
            if (event != nil) {
                [NSApp sendEvent:event];
            }
        }
    }
}

std::string evaluate(WKWebView* webView, const std::string& script)
{
    std::string result;
    std::string* answer = &result;
    bool done = false;
    bool* finished = &done;
    [webView evaluateJavaScript:@(script.c_str())
              completionHandler:^(id value, NSError* error) {
                if (error != nil) {
                    std::fprintf(stderr, "script failed: %s\n  %s\n",
                                 error.localizedDescription.UTF8String, script.c_str());
                }
                if ([value isKindOfClass:[NSString class]]) {
                    *answer = static_cast<NSString*>(value).UTF8String;
                } else if ([value isKindOfClass:[NSNumber class]]) {
                    *answer = static_cast<NSNumber*>(value).stringValue.UTF8String;
                }
                *finished = true;
              }];
    waitUntil([&] { return done; }, 5.0);
    return result;
}

// Two animation frames, so whatever the last script changed has been drawn.
void settle(WKWebView* webView)
{
    bool done = false;
    bool* finished = &done;
    [webView callAsyncJavaScript:
                 @"await new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)));"
        arguments:@{}
        inFrame:nil
        inContentWorld:WKContentWorld.pageWorld
        completionHandler:^(id, NSError*) {
          *finished = true;
        }];
    waitUntil([&] { return done; }, 2.0);
}

CGImageRef snapshot(WKWebView* webView)
{
    CGImageRef image = nullptr;
    CGImageRef* filmed = &image;
    bool done = false;
    bool* finished = &done;
    WKSnapshotConfiguration* configuration = [[WKSnapshotConfiguration alloc] init];
    configuration.afterScreenUpdates = YES;
    [webView takeSnapshotWithConfiguration:configuration
                         completionHandler:^(NSImage* taken, NSError* error) {
                           if (error != nil) {
                               std::fprintf(stderr, "snapshot failed: %s\n",
                                            error.localizedDescription.UTF8String);
                           }
                           if (taken != nil) {
                               *filmed = CGImageRetain([taken CGImageForProposedRect:nil
                                                                             context:nil
                                                                               hints:nil]);
                           }
                           *finished = true;
                         }];
    waitUntil([&] { return done; }, 5.0);
    return image;
}

// Helpers the script calls in the page: find controls by text, click them, and send the pointer
// and wheel events a user's mouse would. Coordinates are CSS pixels in the page.
const char* const kPageHelpers = R"JS(
window.showcase = {
  now: 0,
  find(selector, text) {
    const all = [...document.querySelectorAll(selector)];
    return text === undefined ? all[0]
      : all.find((node) => node.textContent.trim().toLowerCase().startsWith(text.toLowerCase()));
  },
  centre(selector, text) {
    const node = this.find(selector, text);
    if (!node) return "";
    const box = node.getBoundingClientRect();
    return JSON.stringify([box.left + box.width / 2, box.top + box.height / 2]);
  },
  box(selector) {
    const box = this.find(selector)?.getBoundingClientRect();
    return box ? JSON.stringify([box.left, box.top, box.width, box.height]) : "";
  },
  click(selector, text) { this.find(selector, text)?.click(); return ""; },
  trace() { return document.getElementById("trace"); },
  hover(x, y) {
    this.trace().dispatchEvent(new PointerEvent("pointermove",
      { clientX: x, clientY: y, buttons: 0, bubbles: true }));
    return "";
  },
  leave() {
    document.getElementById("screen").dispatchEvent(new PointerEvent("pointerleave"));
    return "";
  },
  zoom(x, y, delta) {
    this.trace().dispatchEvent(new WheelEvent("wheel",
      { clientX: x, clientY: y, deltaY: delta, metaKey: true, bubbles: true, cancelable: true }));
    return "";
  },
  reset() {
    document.getElementById("screen").dispatchEvent(new MouseEvent("dblclick", { bubbles: true }));
    return "";
  },
  // A drag of a cursor line. The page captures the pointer, which a script's pointer cannot be;
  // the capture is skipped while the drag starts.
  grab(selector, x, y) {
    this.held = this.find(selector);
    const capture = Element.prototype.setPointerCapture;
    Element.prototype.setPointerCapture = () => {};
    this.held?.dispatchEvent(new PointerEvent("pointerdown",
      { clientX: x, clientY: y, buttons: 1, pointerId: 7, bubbles: true }));
    Element.prototype.setPointerCapture = capture;
    return "";
  },
  drag(x, y) {
    this.held?.dispatchEvent(new PointerEvent("pointermove",
      { clientX: x, clientY: y, buttons: 1, pointerId: 7, bubbles: true }));
    return "";
  },
  release(x, y) {
    this.held?.dispatchEvent(new PointerEvent("pointerup",
      { clientX: x, clientY: y, pointerId: 7, bubbles: true }));
    this.held = undefined;
    return "";
  },
};
// The page's clock follows the film, so readouts refresh as often as in real time.
performance.now = () => window.showcase.now;
""
)JS";

// ---------------------------------------------------------------------------------------------
// Drawing. The context is flipped so that y grows downwards, as in the page.

CGColorRef rgba(double r, double g, double b, double a)
{
    static CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    const std::array<CGFloat, 4> components{r, g, b, a};
    return CGColorCreate(space, components.data());
}

void fill(CGContextRef context, CGPathRef path, double r, double g, double b, double a)
{
    CGColorRef colour = rgba(r, g, b, a);
    CGContextSetFillColorWithColor(context, colour);
    CGContextAddPath(context, path);
    CGContextFillPath(context);
    CGColorRelease(colour);
}

// An image drawn upright into `rect` of the flipped context.
void drawImage(CGContextRef context, CGImageRef image, CGRect rect)
{
    CGContextSaveGState(context);
    CGContextTranslateCTM(context, rect.origin.x, rect.origin.y + rect.size.height);
    CGContextScaleCTM(context, 1, -1);
    CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
    CGContextDrawImage(context, CGRectMake(0, 0, rect.size.width, rect.size.height), image);
    CGContextRestoreGState(context);
}

struct Text
{
    std::string text;
    double size = 48;
    NSFontWeight weight = NSFontWeightBold;
    double kern = 0;
    double r = 1, g = 1, b = 1, a = 1;
};

CTLineRef makeLine(const Text& text)
{
    NSFont* font = [NSFont systemFontOfSize:text.size weight:text.weight];
    NSColor* colour = [NSColor colorWithSRGBRed:text.r green:text.g blue:text.b alpha:text.a];
    NSDictionary* attributes = @{
        NSFontAttributeName : font,
        NSForegroundColorAttributeName : colour,
        NSKernAttributeName : @(text.kern),
    };
    NSAttributedString* string = [[NSAttributedString alloc] initWithString:@(text.text.c_str())
                                                                 attributes:attributes];
    return CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)string);
}

double textWidth(const Text& text)
{
    CTLineRef line = makeLine(text);
    const double width = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
    CFRelease(line);
    return width;
}

// Centred on `x`, with its baseline at `y`.
void drawText(CGContextRef context, const Text& text, double x, double y)
{
    CTLineRef line = makeLine(text);
    CGContextSaveGState(context);
    CGContextSetTextMatrix(context, CGAffineTransformMakeScale(1, -1));
    CGContextSetTextPosition(
        context,
        x - CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr) / 2 + text.kern / 2, y);
    CTLineDraw(line, context);
    CGContextRestoreGState(context);
    CFRelease(line);
}

CGImageRef makeBackground()
{
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(
        nullptr, kVideoWidth, kVideoHeight, 8, 0, space,
        static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedFirst) | kCGBitmapByteOrder32Little);
    const std::array<CGFloat, 8> colours{0.10, 0.12, 0.19, 1, 0.027, 0.031, 0.047, 1};
    const std::array<CGFloat, 2> stops{0, 1};
    CGGradientRef gradient =
        CGGradientCreateWithColorComponents(space, colours.data(), stops.data(), 2);
    const CGPoint centre = CGPointMake(kVideoWidth / 2.0, kVideoHeight * 0.55);
    CGContextDrawRadialGradient(context, gradient, centre, 0, centre, kVideoWidth * 0.75,
                                kCGGradientDrawsAfterEndLocation);
    CGImageRef image = CGBitmapContextCreateImage(context);
    CGGradientRelease(gradient);
    CGContextRelease(context);
    CGColorSpaceRelease(space);
    return image;
}

void drawCursor(CGContextRef context, Point at, double alpha, double press, bool held)
{
    if (alpha <= 0.0) {
        return;
    }
    CGContextSaveGState(context);
    CGContextSetAlpha(context, alpha);
    if (press > 0.0 || held) {
        // A click: a ring that grows and fades. A held button: a steady ring.
        CGColorRef ring = rgba(1, 1, 1, held ? 0.45 : 0.6 * (1 - press));
        CGContextSetStrokeColorWithColor(context, ring);
        CGContextSetLineWidth(context, 3);
        const double radius = held ? 16 : 12 + 30 * easeOut(press);
        CGContextStrokeEllipseInRect(
            context, CGRectMake(at.x - radius, at.y - radius, 2 * radius, 2 * radius));
        CGColorRelease(ring);
    }
    CGContextTranslateCTM(context, at.x, at.y);
    CGContextScaleCTM(context, 1.7, 1.7);
    CGMutablePathRef arrow = CGPathCreateMutable();
    const std::array<CGPoint, 7> points{{{.x = 0, .y = 0},
                                         {.x = 0, .y = 17},
                                         {.x = 4, .y = 13.2},
                                         {.x = 6.8, .y = 19.6},
                                         {.x = 9.6, .y = 18.4},
                                         {.x = 6.9, .y = 12.2},
                                         {.x = 12.2, .y = 12.2}}};
    CGPathAddLines(arrow, nullptr, points.data(), points.size());
    CGPathCloseSubpath(arrow);
    CGColorRef shadow = rgba(0, 0, 0, 0.5);
    CGContextSetShadowWithColor(context, CGSizeMake(0, 2), 4, shadow);
    fill(context, arrow, 0.05, 0.05, 0.05, 1);
    CGContextSetShadowWithColor(context, CGSizeZero, 0, nullptr);
    CGColorRelease(shadow);
    CGColorRef white = rgba(1, 1, 1, 1);
    CGContextSetStrokeColorWithColor(context, white);
    CGContextSetLineWidth(context, 1.3);
    CGContextSetLineJoin(context, kCGLineJoinRound);
    CGContextAddPath(context, arrow);
    CGContextStrokePath(context);
    CGColorRelease(white);
    CGPathRelease(arrow);
    CGContextRestoreGState(context);
}

enum class Place : std::uint8_t
{
    kTop,
    kBottom,
};

// A caption over the page: a headline and a line explaining it, on a dark panel, rising in and
// fading out.
void drawCaption(CGContextRef context, const std::string& headline, const std::string& detail,
                 Place place, double visible)
{
    if (visible <= 0.0) {
        return;
    }
    const Text title{
        .text = headline, .size = 50, .weight = NSFontWeightBold, .kern = -0.6, .a = visible};
    const Text line{.text = detail,
                    .size = 28,
                    .weight = NSFontWeightMedium,
                    .r = 0.80,
                    .g = 0.84,
                    .b = 0.90,
                    .a = visible};
    const double width = std::max(textWidth(title), textWidth(line)) + 96;
    const double height = 142;
    const double top =
        (place == Place::kTop ? 96 : kVideoHeight - 290 - height) + 16 * (1 - visible);
    const CGRect panel = CGRectMake(kVideoWidth / 2.0 - width / 2, top, width, height);
    CGPathRef shape = CGPathCreateWithRoundedRect(panel, 22, 22, nullptr);
    CGContextSaveGState(context);
    CGColorRef shadow = rgba(0, 0, 0, 0.5 * visible);
    CGContextSetShadowWithColor(context, CGSizeMake(0, 10), 40, shadow);
    fill(context, shape, 0.035, 0.04, 0.06, 0.86 * visible);
    CGContextSetShadowWithColor(context, CGSizeZero, 0, nullptr);
    CGColorRelease(shadow);
    CGColorRef edge = rgba(1, 1, 1, 0.12 * visible);
    CGContextSetStrokeColorWithColor(context, edge);
    CGContextSetLineWidth(context, 1.5);
    CGContextAddPath(context, shape);
    CGContextStrokePath(context);
    CGColorRelease(edge);
    CGContextRestoreGState(context);
    CGPathRelease(shape);
    drawText(context, title, kVideoWidth / 2.0, top + 66);
    drawText(context, line, kVideoWidth / 2.0, top + 112);
}

void drawLogo(CGContextRef context, double alpha, double y, const std::string& line)
{
    if (alpha <= 0.0) {
        return;
    }
    drawText(context,
             {.text = "SINELESS", .size = 136, .weight = NSFontWeightHeavy, .kern = 24, .a = alpha},
             kVideoWidth / 2.0, y);
    drawText(context,
             {.text = line,
              .size = 36,
              .weight = NSFontWeightMedium,
              .kern = 1,
              .r = 0.72,
              .g = 0.76,
              .b = 0.84,
              .a = alpha},
             kVideoWidth / 2.0, y + 78);
}

// The camera: how much the page is magnified and which point of it (CSS pixels) is centred.
// The point is kept far enough from the edges that the page always fills the frame.
struct Camera
{
    double zoom = 1.0;
    Point focus{.x = kEditorWidth / 2, .y = kEditorHeight / 2};
};

Camera framed(Camera camera)
{
    const double halfWidth = kEditorWidth / 2 / camera.zoom;
    const double halfHeight = kEditorHeight / 2 / camera.zoom;
    camera.focus.x = std::clamp(camera.focus.x, halfWidth, kEditorWidth - halfWidth);
    camera.focus.y = std::clamp(camera.focus.y, halfHeight, kEditorHeight - halfHeight);
    return camera;
}

// Page coordinates to video coordinates; `settle` scales the page about the frame's centre as it
// arrives.
Point toVideo(const Camera& camera, double settle, Point p)
{
    const double scale = kPageScale * camera.zoom * settle;
    return {.x = kVideoWidth / 2.0 + (p.x - camera.focus.x) * scale,
            .y = kVideoHeight / 2.0 + (p.y - camera.focus.y) * scale};
}

void drawPage(CGContextRef context, CGImageRef page, const Camera& camera, double settle,
              double opacity)
{
    if (page == nullptr || opacity <= 0.0) {
        return;
    }
    const Point origin = toVideo(camera, settle, {.x = 0, .y = 0});
    const double scale = kPageScale * camera.zoom * settle;
    CGContextSaveGState(context);
    CGContextSetAlpha(context, opacity);
    drawImage(context, page,
              CGRectMake(origin.x, origin.y, kEditorWidth * scale, kEditorHeight * scale));
    CGContextRestoreGState(context);
}

// ---------------------------------------------------------------------------------------------
// Encoding.

class Encoder
{
public:
    explicit Encoder(const std::string& path)
    {
        NSURL* url = [NSURL fileURLWithPath:@(path.c_str())];
        [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
        [[NSFileManager defaultManager] createDirectoryAtURL:[url URLByDeletingLastPathComponent]
                                 withIntermediateDirectories:YES
                                                  attributes:nil
                                                       error:nil];
        NSError* error = nil;
        writer = [[AVAssetWriter alloc] initWithURL:url fileType:AVFileTypeMPEG4 error:&error];
        if (writer == nil) {
            throw std::runtime_error(error.localizedDescription.UTF8String);
        }
        NSDictionary* videoSettings = @{
            AVVideoCodecKey : AVVideoCodecTypeH264,
            AVVideoWidthKey : @(kVideoWidth),
            AVVideoHeightKey : @(kVideoHeight),
            AVVideoColorPropertiesKey : @{
                AVVideoColorPrimariesKey : AVVideoColorPrimaries_ITU_R_709_2,
                AVVideoTransferFunctionKey : AVVideoTransferFunction_ITU_R_709_2,
                AVVideoYCbCrMatrixKey : AVVideoYCbCrMatrix_ITU_R_709_2,
            },
            AVVideoCompressionPropertiesKey : @{
                AVVideoAverageBitRateKey : @(24'000'000),
                AVVideoProfileLevelKey : AVVideoProfileLevelH264HighAutoLevel,
                AVVideoExpectedSourceFrameRateKey : @(kFps),
                AVVideoMaxKeyFrameIntervalKey : @(kFps),
            },
        };
        video = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo
                                               outputSettings:videoSettings];
        video.expectsMediaDataInRealTime = NO;
        adaptor = [[AVAssetWriterInputPixelBufferAdaptor alloc]
               initWithAssetWriterInput:video
            sourcePixelBufferAttributes:@{
                (NSString*)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
                (NSString*)kCVPixelBufferWidthKey : @(kVideoWidth),
                (NSString*)kCVPixelBufferHeightKey : @(kVideoHeight),
            }];
        AudioChannelLayout layout{};
        layout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
        audio = [[AVAssetWriterInput alloc]
            initWithMediaType:AVMediaTypeAudio
               outputSettings:@{
                   AVFormatIDKey : @(kAudioFormatMPEG4AAC),
                   AVSampleRateKey : @(kRate),
                   AVNumberOfChannelsKey : @2,
                   AVEncoderBitRateKey : @256000,
                   AVChannelLayoutKey : [NSData dataWithBytes:&layout length:sizeof(layout)],
               }];
        audio.expectsMediaDataInRealTime = NO;
        [writer addInput:video];
        [writer addInput:audio];
        [writer startWriting];
        [writer startSessionAtSourceTime:kCMTimeZero];
        AudioStreamBasicDescription format{.mSampleRate = kRate,
                                           .mFormatID = kAudioFormatLinearPCM,
                                           .mFormatFlags =
                                               kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
                                           .mBytesPerPacket = 8,
                                           .mFramesPerPacket = 1,
                                           .mBytesPerFrame = 8,
                                           .mChannelsPerFrame = 2,
                                           .mBitsPerChannel = 32,
                                           .mReserved = 0};
        CMAudioFormatDescriptionCreate(nullptr, &format, 0, nullptr, 0, nullptr, nullptr,
                                       &audioFormat);
    }
    ~Encoder() { CFRelease(audioFormat); }
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    // A buffer to draw frame `index` into; append() sends it.
    CVPixelBufferRef frame()
    {
        CVPixelBufferRef buffer = nullptr;
        CVPixelBufferPoolCreatePixelBuffer(nullptr, adaptor.pixelBufferPool, &buffer);
        return buffer;
    }

    // While the picture waits, `feed` is called to append more sound: the writer interleaves the
    // two and holds the picture back until the sound, which its encoder delays, is ahead of it.
    void append(CVPixelBufferRef buffer, int index, const std::function<bool()>& feed)
    {
        waitUntil(
            [&] {
                if (video.readyForMoreMediaData) {
                    return true;
                }
                if (audio.readyForMoreMediaData && !audioFinished && !feed()) {
                    [audio markAsFinished]; // No more sound: the picture need not wait for it.
                    audioFinished = true;
                }
                return false;
            },
            10.0);
        if (!video.readyForMoreMediaData || ![adaptor appendPixelBuffer:buffer
                                                   withPresentationTime:CMTimeMake(index, kFps)]) {
            CVPixelBufferRelease(buffer);
            throw std::runtime_error(
                std::string("video frame refused at ") + std::to_string(index) + " (audio ready " +
                std::to_string(audio.readyForMoreMediaData) + ", status " +
                std::to_string(writer.status) +
                "): " + (writer.error ? writer.error.description.UTF8String : "not ready"));
        }
        CVPixelBufferRelease(buffer);
    }

    // Interleaved stereo, starting at sample `start`.
    void appendAudio(const std::vector<float>& interleaved, std::int64_t start)
    {
        const std::size_t bytes = interleaved.size() * sizeof(float);
        CMBlockBufferRef block = nullptr;
        CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, bytes, nullptr, nullptr, 0, bytes,
                                           kCMBlockBufferAssureMemoryNowFlag, &block);
        CMBlockBufferReplaceDataBytes(interleaved.data(), block, 0, bytes);
        CMSampleBufferRef sample = nullptr;
        CMAudioSampleBufferCreateReadyWithPacketDescriptions(
            nullptr, block, audioFormat, static_cast<CMItemCount>(interleaved.size() / 2),
            CMTimeMake(start, static_cast<int32_t>(kRate)), nullptr, &sample);
        if (sample == nullptr || !audio.readyForMoreMediaData ||
            ![audio appendSampleBuffer:sample]) {
            throw std::runtime_error(
                std::string("audio refused: ") +
                (writer.error ? writer.error.description.UTF8String : "not ready"));
        }
        CFRelease(sample);
        CFRelease(block);
    }

    void finish()
    {
        [video markAsFinished];
        if (!audioFinished) {
            [audio markAsFinished];
        }
        bool done = false;
        bool* finished = &done;
        [writer finishWritingWithCompletionHandler:^{
          *finished = true;
        }];
        waitUntil([&] { return done; }, 60.0);
        if (writer.status != AVAssetWriterStatusCompleted) {
            std::fprintf(stderr, "encoding failed: %s\n",
                         writer.error.localizedDescription.UTF8String);
        }
    }

private:
    AVAssetWriter* writer = nil;
    AVAssetWriterInput* video = nil;
    AVAssetWriterInput* audio = nil;
    AVAssetWriterInputPixelBufferAdaptor* adaptor = nil;
    CMAudioFormatDescriptionRef audioFormat = nullptr;
    bool audioFinished = false;
};

void set(engine::Engine& engine, protocol::SettingId id, const char* value)
{
    if (const auto parsed = engine::settingFromJson(id, nlohmann::json(value))) {
        engine.settings().set(id, *parsed);
    }
}

// ---------------------------------------------------------------------------------------------

int run(const Options& options)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];

        // The tracks, in the set's order, in their colours. Kick Off is the kick a second
        // microphone hears: the set moves its clip 0.0117 beats (281 samples) later and mutes it,
        // so it is made here from the kick. Group B keeps a track out of the view until needed.
        struct Member
        {
            const char* name;
            std::uint32_t colour;
            const char* group;
        };
        const std::vector<Member> members = {
            {.name = "Kick", .colour = 0xF2453D, .group = "a"},
            {.name = "Kick Off", .colour = 0xB06EE6, .group = "b"},
            {.name = "Bass", .colour = 0x5468F0, .group = "a"},
            {.name = "Sub", .colour = 0x2EA8F2, .group = "a"},
            {.name = "Clap", .colour = 0xF2792E, .group = "a"},
            {.name = "Hat", .colour = 0xF5DE47, .group = "a"},
            {.name = "Pluck", .colour = 0xEE4FD3, .group = "a"},
            {.name = "Sweep Up", .colour = 0xA8ADB8, .group = "b"},
        };
        const std::string base = options.stems + "/" + options.prefix;
        std::vector<Stereo> tracks;
        for (const Member& member : members) {
            if (std::string(member.name) == "Kick Off") {
                const Stereo& kick = tracks.front();
                constexpr std::size_t kDelay = 281;
                Stereo late{.left = kick.left, .right = kick.right};
                for (std::size_t i = 0; i < kick.size(); ++i) {
                    late.left[(i + kDelay) % kick.size()] = kick.left[i];
                    late.right[(i + kDelay) % kick.size()] = kick.right[i];
                }
                tracks.push_back(std::move(late));
            } else {
                tracks.push_back(load(base + " " + member.name + ".wav"));
            }
        }
        const Stereo mixdown = load(base + ".wav");
        const std::size_t songLength = mixdown.size();

        std::vector<std::shared_ptr<engine::Engine>> engines;
        for (std::size_t i = 0; i < members.size(); ++i) {
            auto engine = std::make_shared<engine::Engine>();
            engine->prepare(kRate);
            engine->setTrackInfo({.name = members[i].name,
                                  .colour = members[i].colour,
                                  .index = static_cast<int>(i)});
            set(*engine, protocol::SettingId::kGroup, members[i].group);
            engine::InstanceRegistry::shared().add(engine);
            engines.push_back(engine);
        }
        engine::Engine& own = *engines.front();
        using protocol::SettingId;
        set(own, SettingId::kDisplayMode, "single");
        set(own, SettingId::kTimeBase, "beats");
        set(own, SettingId::kBeatSpan, "oneBar");
        set(own, SettingId::kViewLayout, "stacked");

        // The window goes on the sharpest screen, so the page is filmed at its backing scale.
        NSScreen* screen = NSScreen.mainScreen;
        for (NSScreen* candidate in NSScreen.screens) {
            if (candidate.backingScaleFactor > screen.backingScaleFactor) {
                screen = candidate;
            }
        }
        const NSRect visible = screen.visibleFrame;
        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(visible.origin.x + 20,
                                           visible.origin.y + visible.size.height - kEditorHeight -
                                               40,
                                           kEditorWidth, kEditorHeight)
                      styleMask:NSWindowStyleMaskTitled
                        backing:NSBackingStoreBuffered
                          defer:NO
                         screen:screen];
        window.releasedWhenClosed = NO;
        window.title = @"Sineless showcase (filming)";
        NSView* content =
            [[NSView alloc] initWithFrame:NSMakeRect(0, 0, kEditorWidth, kEditorHeight)];
        window.contentView = content;
        window.level = NSFloatingWindowLevel;
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        IPtr<editor::Editor> editor = owned(new editor::Editor(engines.front()));
        ViewRect rect(0, 0, static_cast<int32>(kEditorWidth), static_cast<int32>(kEditorHeight));
        editor->onSize(&rect);
        @autoreleasepool {
            editor->attached((__bridge void*)content, kPlatformTypeNSView);
        }
        auto* webView = (__bridge WKWebView*)editor->webView()->nativeView();
        waitUntil(
            [&] { return evaluate(webView, "String(typeof window.orizonDebug)") == "object"; },
            10.0);
        evaluate(webView, kPageHelpers);

        // The film's state, moved by the script.
        double now = 0.0;
        Track<double> zoom(1.0);
        Track<Point> focus(Point{.x = kEditorWidth / 2, .y = kEditorHeight / 2});
        Track<Point> cursor(Point{.x = kEditorWidth * 0.5, .y = kEditorHeight * 0.55});
        double cursorShown = -1.0; // When the cursor appears; it fades in.
        double pressAt = -10.0;
        bool live = true;      // Frames still arrive (not frozen).
        bool hovering = false; // The pointer is over the traces, so the page hears it move.
        bool dragging = false; // A cursor line is held.
        double zoomUntil = -1.0;
        double zoomRate = 0.0; // Wheel delta per frame while zooming.

        const auto quote = [](const std::string& text) { return "'" + text + "'"; };
        const auto json = [&](const std::string& call) {
            const std::string found = evaluate(webView, "window.showcase." + call);
            const auto value = nlohmann::json::parse(found.empty() ? "[]" : found, nullptr, false);
            return value.is_array() ? value : nlohmann::json::array();
        };
        const auto centre = [&](const std::string& selector, const std::string& text = "") {
            const auto value =
                json("centre(" + quote(selector) + (text.empty() ? "" : "," + quote(text)) + ")");
            if (value.size() < 2) {
                std::fprintf(stderr, "not found: %s %s\n", selector.c_str(), text.c_str());
                return Point{.x = kEditorWidth / 2, .y = kEditorHeight / 2};
            }
            return Point{.x = value[0].get<double>(), .y = value[1].get<double>()};
        };
        const auto box = [&](const std::string& selector) {
            const auto value = json("box(" + quote(selector) + ")");
            return value.size() < 4 ? CGRectMake(0, 0, kEditorWidth, kEditorHeight)
                                    : CGRectMake(value[0].get<double>(), value[1].get<double>(),
                                                 value[2].get<double>(), value[3].get<double>());
        };
        const auto pageCall = [&](const std::string& call) {
            evaluate(webView, "window.showcase." + call);
        };
        const auto at = [](Point p) { return std::to_string(p.x) + "," + std::to_string(p.y); };
        const auto moveTo = [&](Point target, double duration) {
            cursor.move(now, target, duration);
        };
        const auto click = [&](const std::string& selector, const std::string& text = "") {
            pressAt = now;
            pageCall("click(" + quote(selector) + (text.empty() ? "" : "," + quote(text)) + ")");
        };
        const auto look = [&](double magnify, Point point, double duration) {
            zoom.move(now, magnify, duration);
            focus.move(now, point, duration);
        };
        const Point middle{.x = kEditorWidth / 2, .y = kEditorHeight / 2};
        const auto group = [&](const char* name, const char* value) {
            for (std::size_t i = 0; i < members.size(); ++i) {
                if (std::string(members[i].name) == name) {
                    set(*engines[i], SettingId::kGroup, value);
                }
            }
        };
        const auto grab = [&](const std::string& selector) {
            pageCall("grab(" + quote(selector) + "," + at(cursor.at(now)) + ")");
            dragging = true;
        };
        const auto release = [&] {
            pageCall("release(" + at(cursor.at(now)) + ")");
            dragging = false;
        };

        // The script: when, and what.
        struct Cue
        {
            double at;
            std::function<void()> run;
        };
        CGRect trace{};
        Point kickHit{};
        std::vector<Cue> cues = {
            // Single: the page arrives as the drop lands.
            {.at = 6.0, .run = [&] { look(1.04, middle, 6.0); }},
            // Multi.
            {.at = 12.0, .run = [&] { look(1.0, middle, 1.0); }},
            {.at = 12.2, .run = [&] { cursorShown = now; }},
            {.at = 12.4, .run = [&] { moveTo(centre("#display-mode button", "multi"), 1.0); }},
            {.at = 13.6, .run = [&] { click("#display-mode button", "multi"); }},
            {.at = 15.0, .run = [&] { look(1.06, {.x = middle.x, .y = middle.y + 20}, 5.0); }},
            // Freeze at the end of a bar, then zoom into the kick's attack.
            {.at = 20.6, .run = [&] { moveTo(centre("#freeze"), 1.0); }},
            {.at = 21.92,
             .run =
                 [&] {
                     click("#freeze");
                     live = false;
                 }},
            {.at = 22.6,
             .run =
                 [&] {
                     trace = box("#trace");
                     const auto lane = centre("#lane-labels span", "kick");
                     kickHit = {.x = trace.origin.x + 3, .y = lane.y + 30};
                     moveTo(kickHit, 1.0);
                     look(1.6, {.x = kickHit.x + 320, .y = kickHit.y + 40}, 1.4);
                 }},
            {.at = 24.4,
             .run =
                 [&] {
                     zoomUntil = 28.6;
                     zoomRate = -1.1;
                 }},
            {.at = 29.6, .run = [&] { look(1.0, middle, 1.0); }},
            {.at = 30.0, .run = [&] { pageCall("reset()"); }},
            // Hover.
            {.at = 31.0, .run = [&] { moveTo(centre("#hover-toggle"), 1.0); }},
            {.at = 32.2, .run = [&] { click("#hover-toggle"); }},
            {.at = 32.8,
             .run =
                 [&] {
                     const auto sub = centre("#lane-labels span", "sub");
                     moveTo({.x = kEditorWidth * 0.22, .y = sub.y + 30}, 0.9);
                     look(1.25, {.x = middle.x, .y = sub.y - 40}, 1.2);
                 }},
            {.at = 33.8,
             .run =
                 [&] {
                     hovering = true;
                     moveTo({.x = kEditorWidth * 0.66, .y = cursor.target().y - 8}, 4.4);
                 }},
            {.at = 38.4,
             .run =
                 [&] {
                     hovering = false;
                     pageCall("leave()");
                     pageCall("click('#hover-toggle')");
                     look(1.0, middle, 1.0);
                 }},
            // Time cursors: A onto the bar's first kick, B a beat later.
            {.at = 38.8, .run = [&] { moveTo(centre("#time-cursors"), 1.0); }},
            {.at = 40.0, .run = [&] { click("#time-cursors"); }},
            {.at = 40.5,
             .run =
                 [&] {
                     moveTo({.x = centre("#cursor-a").x,
                             .y = trace.origin.y + trace.size.height * 0.42},
                            0.8);
                 }},
            {.at = 41.5,
             .run =
                 [&] {
                     grab("#cursor-a");
                     moveTo({.x = trace.origin.x + 2, .y = cursor.target().y}, 1.2);
                 }},
            {.at = 42.9, .run = [&] { release(); }},
            {.at = 43.1,
             .run =
                 [&] {
                     moveTo({.x = centre("#cursor-b").x,
                             .y = trace.origin.y + trace.size.height * 0.42},
                            0.8);
                 }},
            {.at = 44.0,
             .run =
                 [&] {
                     grab("#cursor-b");
                     moveTo(
                         {.x = trace.origin.x + trace.size.width / 4 + 2, .y = cursor.target().y},
                         1.4);
                 }},
            {.at = 45.6, .run = [&] { release(); }},
            // Level cursors: the first onto the kick's peak.
            {.at = 46.4, .run = [&] { moveTo(centre("#level-cursors"), 0.9); }},
            {.at = 47.5, .run = [&] { click("#level-cursors"); }},
            {.at = 48.0,
             .run =
                 [&] {
                     const auto line = box(".level-cursor");
                     moveTo({.x = line.origin.x + line.size.width * 0.55,
                             .y = line.origin.y + line.size.height / 2},
                            0.8);
                 }},
            {.at = 49.0,
             .run =
                 [&] {
                     grab(".level-cursor");
                     const double lane = trace.size.height / 6;
                     moveTo({.x = cursor.target().x,
                             .y = trace.origin.y + lane * 0.5 - lane * 0.5 * 0.72},
                            1.4);
                 }},
            {.at = 50.6, .run = [&] { release(); }},
            // Compare: the two kick microphones alone.
            {.at = 52.4,
             .run =
                 [&] {
                     pageCall("click('#time-cursors')");
                     pageCall("click('#level-cursors')");
                     pageCall("click('#freeze')");
                     live = true;
                     for (const char* name : {"Bass", "Sub", "Clap", "Hat", "Pluck"}) {
                         group(name, "b");
                     }
                     group("Kick Off", "a");
                     look(1.0, middle, 1.0);
                 }},
            {.at = 52.8, .run = [&] { moveTo(centre("#display-mode button", "compare"), 1.0); }},
            {.at = 54.0, .run = [&] { click("#display-mode button", "compare"); }},
            {.at = 55.0,
             .run =
                 [&] {
                     const auto out = centre("#lane-labels span", "kick off");
                     moveTo({.x = out.x + 70, .y = out.y + 2}, 0.9);
                     look(1.35, {.x = kEditorWidth * 0.33, .y = kEditorHeight * 0.45}, 1.4);
                 }},
            {.at = 57.4, .run = [&] { moveTo(centre(".preview-fix"), 0.8); }},
            {.at = 58.4, .run = [&] { click(".preview-fix"); }},
            {.at = 60.6, .run = [&] { look(1.0, middle, 0.8); }},
        };
        std::size_t nextCue = 0;

        struct Caption
        {
            double from;
            double to;
            Place place;
            const char* headline;
            const char* detail;
        };
        const std::array<Caption, 7> captions{{
            {.from = 6.8,
             .to = 12.0,
             .place = Place::kBottom,
             .headline = "A real-time oscilloscope for your DAW",
             .detail = "Insert it on any track to watch the waveform as it plays."},
            {.from = 14.2,
             .to = 20.8,
             .place = Place::kBottom,
             .headline = "Every track, lined up on the beat",
             .detail =
                 "Each track with Sineless gets its own lane, in its colour, on one timeline."},
            {.from = 22.4,
             .to = 31.0,
             .place = Place::kBottom,
             .headline = "Freeze, then zoom down to the sample",
             .detail = "⌘ + scroll zooms around the pointer. Double-click to zoom back out."},
            {.from = 32.6,
             .to = 38.4,
             .place = Place::kTop,
             .headline = "Read time and level anywhere",
             .detail = "Turn on Hover and point at a trace."},
            {.from = 40.3,
             .to = 46.2,
             .place = Place::kBottom,
             .headline = "Measure with time cursors",
             .detail = "Drag A and B to read the time and frequency between them."},
            {.from = 47.8,
             .to = 52.4,
             .place = Place::kBottom,
             .headline = "…and level cursors",
             .detail = "Drag them onto peaks to read levels, and their difference, in dB."},
            {.from = 54.4,
             .to = 60.8,
             .place = Place::kTop,
             .headline = "Spot phase problems between tracks",
             .detail = "Compare finds each track's offset and polarity, and previews the fix."},
        }};

        Encoder encoder(options.out);
        CGImageRef background = makeBackground();
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        const auto songStart = static_cast<std::uint64_t>((kSongStart - kPreroll) * kRate);
        std::uint64_t sample = 0; // Played so far, from the preroll's start.
        double framesDrawn = 0.0;
        const auto drawn = [&] {
            const std::string text =
                evaluate(webView, "String(window.orizonDebug.perf.framesDrawn)");
            return text.empty() ? 0.0 : std::stod(text);
        };
        // One frame of audio into every engine, on one host clock.
        const auto play = [&] {
            std::vector<float> left(kBlock);
            std::vector<float> right(kBlock);
            const double seconds =
                kSetStart + kSongStart - kPreroll + static_cast<double>(sample) / kRate;
            for (std::size_t track = 0; track < engines.size(); ++track) {
                for (int i = 0; i < kBlock; ++i) {
                    const auto index =
                        (songStart + sample + static_cast<std::uint64_t>(i)) % songLength;
                    left[static_cast<std::size_t>(i)] = tracks[track].left[index];
                    right[static_cast<std::size_t>(i)] = tracks[track].right[index];
                }
                std::array<const float*, 2> channels{left.data(), right.data()};
                engines[track]->captureRing().write(channels.data(), 2, kBlock);
                engines[track]->timeline().advance({.playing = true,
                                                    .positionValid = true,
                                                    .quarterNotes = seconds * kTempo / 60.0,
                                                    .tempoValid = true,
                                                    .tempo = kTempo,
                                                    .timeSignatureValid = true,
                                                    .numerator = 4,
                                                    .denominator = 4,
                                                    .secondsValid = true,
                                                    .seconds = seconds},
                                                   kBlock, kRate);
            }
            sample += static_cast<std::uint64_t>(kBlock);
        };
        const auto step = [&](double pageTime) {
            evaluate(webView,
                     "window.showcase.now = " + std::to_string(pageTime * 1000.0) + "; ''");
            play();
            if (live) {
                const double before = framesDrawn;
                waitUntil([&] { return (framesDrawn = drawn()) > before; }, 0.5);
            }
        };

        // Preroll: a bar plays before filming starts, so the sweep is full.
        const int prerollFrames = static_cast<int>(kPreroll * kFps);
        for (int f = 0; f < prerollFrames; ++f) {
            step(static_cast<double>(f) / kFps);
        }

        const int total = options.frames > 0 ? options.frames : static_cast<int>(kSeconds * kFps);
        // The soundtrack for video frame `index`: the song's mix, faded in and out.
        int soundFrames = 0;
        const auto appendSound = [&](int index) {
            std::vector<float> mix(std::size_t{2} * kBlock);
            for (int i = 0; i < kBlock; ++i) {
                const double t = static_cast<double>(index) / kFps + static_cast<double>(i) / kRate;
                const double fade = std::min(clamp01(t / 1.0), clamp01((kSeconds - t) / 2.5));
                const auto position =
                    (songStart + static_cast<std::uint64_t>(std::llround((kPreroll + t) * kRate))) %
                    songLength;
                mix[2 * static_cast<std::size_t>(i)] =
                    static_cast<float>(fade * mixdown.left[position]);
                mix[2 * static_cast<std::size_t>(i) + 1] =
                    static_cast<float>(fade * mixdown.right[position]);
            }
            encoder.appendAudio(mix, static_cast<std::int64_t>(index) * kBlock);
        };

        const auto started = std::chrono::steady_clock::now();
        for (int f = 0; f < total; ++f) {
            now = static_cast<double>(f) / kFps;
            step(kPreroll + now);
            while (nextCue < cues.size() && cues[nextCue].at <= now + 1e-9) {
                cues[nextCue++].run();
            }
            const Point pointer = cursor.at(now);
            if (now < zoomUntil) {
                pageCall("zoom(" + at(pointer) + "," + std::to_string(zoomRate) + ")");
            }
            if (hovering) {
                pageCall("hover(" + at(pointer) + ")");
            }
            if (dragging) {
                pageCall("drag(" + at(pointer) + ")");
            }
            settle(webView);
            CGImageRef page = snapshot(webView);
            if (f == 0 && page != nullptr) {
                std::printf("page filmed at %zux%zu\n", CGImageGetWidth(page),
                            CGImageGetHeight(page));
            }

            CVPixelBufferRef buffer = encoder.frame();
            CVPixelBufferLockBaseAddress(buffer, 0);
            CGContextRef context =
                CGBitmapContextCreate(CVPixelBufferGetBaseAddress(buffer), kVideoWidth,
                                      kVideoHeight, 8, CVPixelBufferGetBytesPerRow(buffer), space,
                                      static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedFirst) |
                                          kCGBitmapByteOrder32Little);
            CGContextTranslateCTM(context, 0, kVideoHeight);
            CGContextScaleCTM(context, 1, -1);
            drawImage(context, background, CGRectMake(0, 0, kVideoWidth, kVideoHeight));

            // Intro: the name while the sweep rises, then the page as the drop lands. Outro: the
            // page gives way to the name.
            drawLogo(context, shown(now, 0.4, 5.4, 0.8, 0.6), 520 - 30 * smooth((now - 4.8) / 0.6),
                     "by Orizon Audio");
            const double arrive = easeOut((now - 5.2) / 1.4);
            const double opacity = smooth((now - 5.2) / 0.8) * (1 - smooth((now - 61.0) / 0.8));
            const double settleScale = 1.05 - 0.05 * arrive + 0.03 * smooth((now - 61.0) / 0.8);
            const Camera view = framed({.zoom = zoom.at(now), .focus = focus.at(now)});
            drawPage(context, page, view, settleScale, opacity);
            for (const Caption& caption : captions) {
                drawCaption(context, caption.headline, caption.detail, caption.place,
                            shown(now, caption.from, caption.to, 0.45, 0.45));
            }
            const double cursorAlpha = cursorShown < 0.0 ? 0.0
                                                         : smooth((now - cursorShown) / 0.4) *
                                                               (1 - smooth((now - 60.6) / 0.4));
            const double press = now - pressAt < 0.4 ? (now - pressAt) / 0.4 : 0.0;
            drawCursor(context, toVideo(view, settleScale, pointer), cursorAlpha, press, dragging);
            drawLogo(context, smooth((now - 61.6) / 0.8), 500,
                     "Free and open source  ·  VST3  ·  macOS and Windows");
            const double black = smooth((now - (kSeconds - 0.7)) / 0.7);
            if (black > 0.0) {
                CGColorRef shade = rgba(0, 0, 0, black);
                CGContextSetFillColorWithColor(context, shade);
                CGContextFillRect(context, CGRectMake(0, 0, kVideoWidth, kVideoHeight));
                CGColorRelease(shade);
            }
            CGContextRelease(context);
            CVPixelBufferUnlockBaseAddress(buffer, 0);
            if (page != nullptr) {
                CGImageRelease(page);
            }
            while (soundFrames <= f) {
                appendSound(soundFrames++);
            }
            encoder.append(buffer, f, [&] {
                if (soundFrames >= total) {
                    return false;
                }
                appendSound(soundFrames++);
                return true;
            });

            if (f % kFps == 0) {
                const double elapsed =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
                        .count();
                std::printf("%5.1f s of %.0f filmed (%.0f s so far)\n", now, kSeconds, elapsed);
                std::fflush(stdout);
            }
        }
        encoder.finish();
        CGImageRelease(background);
        CGColorSpaceRelease(space);
        @autoreleasepool {
            editor->removed();
        }
        for (const auto& engine : engines) {
            engine::InstanceRegistry::shared().remove(*engine);
        }
        std::printf("wrote %s\n", options.out.c_str());
    }
    return 0;
}

} // namespace

int main(int argc, const char* argv[])
{
    try {
        return run(parse(std::span(argv, static_cast<std::size_t>(argc))));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "showcase: %s\n", error.what());
        return 1;
    }
}
