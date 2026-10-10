// Showcase video: the real editor page in a real WKWebView, fed by the real engines, filmed one
// video frame at a time. A synthesized groove plays through six instances; a script clicks
// through the main features while a drawn cursor, camera moves and captions frame them; each
// frame is composed with Core Graphics and encoded with the groove as the soundtrack.
//
//   showcase [--out=build/showcase/O-Scope.mp4] [--seconds=30] [--frames=N]
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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <numbers>
#include <random>
#include <string>
#include <vector>

using namespace orizon;
using namespace Steinberg;

namespace {

constexpr double kRate = 48000.0;
constexpr double kTempo = 120.0;
constexpr double kBeat = 60.0 / kTempo;
constexpr int kFps = 60;
constexpr int kBlock = static_cast<int>(kRate) / kFps; // Audio per video frame.
constexpr double kPreroll = 2.0; // Seconds played before the first frame: one bar.

constexpr int kVideoWidth = 1920;
constexpr int kVideoHeight = 1080;
constexpr double kEditorWidth = 1200.0; // CSS pixels.
constexpr double kEditorHeight = 680.0;
constexpr double kTitleBar = 28.0;
constexpr double kWindowHeight = kEditorHeight + kTitleBar;
constexpr double kWindowScale = 1.1; // Video pixels per CSS pixel at rest.
constexpr double kWindowCentreY = 580.0;

struct Options
{
    std::string out = "build/showcase/O-Scope.mp4";
    double seconds = 30.0;
    int frames = -1; // Stop early, for trying out the script.
};

Options parse(int argc, const char* argv[])
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto equals = argument.find('=');
        const std::string key = argument.substr(0, equals);
        const std::string value = equals == std::string::npos ? "" : argument.substr(equals + 1);
        if (key == "--out") {
            options.out = value;
        } else if (key == "--seconds") {
            options.seconds = std::stod(value);
        } else if (key == "--frames") {
            options.frames = std::stoi(value);
        }
    }
    return options;
}

// ---------------------------------------------------------------------------------------------
// The groove: four bars at 120 BPM, one mono signal per track.

using Signal = std::vector<float>;

struct Groove
{
    Signal kickIn, clap, hats, bass, keys, kickOut;
};

Groove makeGroove()
{
    const auto length = static_cast<std::size_t>(kRate * kBeat * 16);
    std::mt19937 random(7);
    std::uniform_real_distribution<double> noise(-1.0, 1.0);
    const double pi = std::numbers::pi;
    const auto at = [](double beat) { return static_cast<std::size_t>(std::lround(beat * kBeat * kRate)); };
    const auto add = [&](Signal& signal, std::size_t start, const std::vector<double>& sound) {
        for (std::size_t i = 0; i < sound.size() && start + i < signal.size(); ++i) {
            signal[start + i] += static_cast<float>(sound[i]);
        }
    };
    const auto kick = [&] {
        std::vector<double> out;
        double phase = 0.0;
        for (int i = 0; i < static_cast<int>(0.45 * kRate); ++i) {
            const double t = i / kRate;
            phase += 2 * pi * (48 + 110 * std::exp(-t * 28)) / kRate;
            out.push_back(0.95 * std::tanh(1.4 * std::sin(phase) * std::exp(-t * 7.5)) +
                          0.3 * std::exp(-t * 400) * std::sin(phase * 3));
        }
        return out;
    };
    const auto clap = [&] {
        std::vector<double> out;
        double low = 0.0;
        for (int i = 0; i < static_cast<int>(0.35 * kRate); ++i) {
            const double t = i / kRate;
            double bursts = 0.0;
            for (const double delay : {0.0, 0.011, 0.022}) {
                bursts += t >= delay ? std::exp(-(t - delay) * 180) : 0.0;
            }
            const double tail = t >= 0.022 ? 0.55 * std::exp(-(t - 0.022) * 16) : 0.0;
            const double n = noise(random);
            low += 0.35 * (n - low);
            out.push_back(0.8 * (n - low) * std::min(1.0, bursts * 0.7 + tail));
        }
        return out;
    };
    const auto hat = [&](bool open) {
        std::vector<double> out;
        double previous = 0.0;
        for (int i = 0; i < static_cast<int>((open ? 0.22 : 0.05) * kRate); ++i) {
            const double n = noise(random);
            out.push_back(0.42 * (n - previous) * std::exp(-(i / kRate) * (open ? 14 : 90)));
            previous = n;
        }
        return out;
    };
    const auto bass = [&](double frequency, double seconds) {
        std::vector<double> out;
        double phase = 0.0;
        double low = 0.0;
        for (int i = 0; i < static_cast<int>(seconds * kRate); ++i) {
            const double t = i / kRate;
            phase = std::fmod(phase + frequency / kRate, 1.0);
            low += (0.04 + 0.25 * std::exp(-t * 12)) * (2 * phase - 1 - low);
            out.push_back(0.9 * low * std::min(1.0, t * 400) * std::exp(-t * 5) *
                          std::min(1.0, (seconds - t) * 200));
        }
        return out;
    };
    const auto pluck = [&](double frequency, double seconds) {
        const auto period = static_cast<std::size_t>(kRate / frequency);
        std::vector<double> ring(period);
        for (double& value : ring) {
            value = noise(random);
        }
        std::vector<double> out;
        for (std::size_t i = 0; i < static_cast<std::size_t>(seconds * kRate); ++i) {
            const double value = ring[i % period];
            ring[i % period] = 0.985 * 0.5 * (value + ring[(i + 1) % period]);
            out.push_back(0.5 * value);
        }
        return out;
    };
    // MIDI note numbers.
    const auto hz = [](int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); };
    const int roots[] = {36, 36, 32, 34};                    // C2 C2 Ab1 Bb1
    const int chords[][4] = {{60, 63, 67, 70}, {60, 63, 67, 74}, {56, 60, 63, 67}, {58, 62, 65, 68}};

    Groove groove;
    for (Signal* signal : {&groove.kickIn, &groove.clap, &groove.hats, &groove.bass, &groove.keys}) {
        signal->assign(length, 0.0f);
    }
    const auto kickSound = kick();
    for (int bar = 0; bar < 4; ++bar) {
        const double b = bar * 4.0;
        for (const double beat : bar % 2 == 1 ? std::vector<double>{0, 2.5, 3} : std::vector<double>{0, 2}) {
            add(groove.kickIn, at(b + beat), kickSound);
        }
        for (const double beat : {1.0, 3.0}) {
            add(groove.clap, at(b + beat), clap());
        }
        for (int k = 0; k < 8; ++k) {
            add(groove.hats, at(b + k * 0.5), hat(k == 3 || k == 7));
        }
        const std::pair<double, double> notes[] = {{0, 0.4}, {0.75, 0.2}, {1.5, 0.4}, {2.5, 0.2}, {3, 0.5}};
        for (const auto& [beat, beats] : notes) {
            add(groove.bass, at(b + beat), bass(2 * hz(roots[bar]), beats * kBeat));
        }
        const double steps[] = {0, 0.75, 1.5, 2.25, 3, 3.5};
        for (int k = 0; k < 6; ++k) {
            add(groove.keys, at(b + steps[k]), pluck(hz(chords[bar][k % 4]), 0.3));
        }
    }
    for (Signal* signal : {&groove.kickIn, &groove.clap, &groove.hats, &groove.bass, &groove.keys}) {
        float peak = 0.0f;
        for (const float value : *signal) {
            peak = std::max(peak, std::abs(value));
        }
        for (float& value : *signal) {
            value *= 0.89f / std::max(peak, 1e-6f);
        }
    }
    // A second microphone on the kick: further away (1.1 ms later), darker, and wired the other
    // way round, so it partly cancels the first.
    groove.kickOut.assign(length, 0.0f);
    double low = 0.0;
    const std::size_t delay = 53;
    for (std::size_t i = 0; i < length; ++i) {
        low += 0.2 * (groove.kickIn[i] - low);
        groove.kickOut[(i + delay) % length] = static_cast<float>(-0.85 * low);
    }
    return groove;
}

// ---------------------------------------------------------------------------------------------
// Easing and keyframes.

double clamp01(double x) { return std::clamp(x, 0.0, 1.0); }
double smooth(double x) // Ease in and out.
{
    x = clamp01(x);
    return x < 0.5 ? 4 * x * x * x : 1 - std::pow(-2 * x + 2, 3) / 2;
}
double easeOut(double x) { return 1 - std::pow(1 - clamp01(x), 3); }
double lerp(double a, double b, double x) { return a + (b - a) * x; }
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
    T at(double t) const
    {
        const double x = length <= 0.0 ? 1.0 : smooth((t - start) / length);
        return mix(from, to, x);
    }
    T target() const { return to; }

private:
    static double mix(double a, double b, double x) { return lerp(a, b, x); }
    static Point mix(Point a, Point b, double x) { return {lerp(a.x, b.x, x), lerp(a.y, b.y, x)}; }
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
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
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
    [webView callAsyncJavaScript:@"await new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)));"
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
    const CGFloat components[] = {r, g, b, a};
    return CGColorCreate(space, components);
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

double lineWidth(CTLineRef line)
{
    return CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
}

// Centred on `x`, with its baseline at `y`.
void drawText(CGContextRef context, const Text& text, double x, double y)
{
    CTLineRef line = makeLine(text);
    CGContextSaveGState(context);
    CGContextSetTextMatrix(context, CGAffineTransformMakeScale(1, -1));
    CGContextSetTextPosition(context, x - lineWidth(line) / 2 + text.kern / 2, y);
    CTLineDraw(line, context);
    CGContextRestoreGState(context);
    CFRelease(line);
}

CGImageRef makeBackground()
{
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(nullptr, kVideoWidth, kVideoHeight, 8, 0, space,
                                                 static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedFirst) |
                                                     kCGBitmapByteOrder32Little);
    const CGFloat colours[] = {0.10, 0.12, 0.19, 1, 0.027, 0.031, 0.047, 1};
    const CGFloat stops[] = {0, 1};
    CGGradientRef gradient = CGGradientCreateWithColorComponents(space, colours, stops, 2);
    CGContextDrawRadialGradient(context, gradient, CGPointMake(kVideoWidth / 2.0, kVideoHeight * 0.6),
                                0, CGPointMake(kVideoWidth / 2.0, kVideoHeight * 0.6),
                                kVideoWidth * 0.75, kCGGradientDrawsAfterEndLocation);
    // Faint coloured glows, in the tracks' colours.
    const double glows[][5] = {{0.25, 0.30, 0.94, 0.31, 0.29}, {0.78, 0.75, 0.24, 0.55, 0.85}};
    for (const auto& glow : glows) {
        const CGFloat glowColours[] = {static_cast<CGFloat>(glow[2]), static_cast<CGFloat>(glow[3]),
                                       static_cast<CGFloat>(glow[4]), 0.10, 0, 0, 0, 0};
        CGGradientRef soft = CGGradientCreateWithColorComponents(space, glowColours, stops, 2);
        const CGPoint centre = CGPointMake(kVideoWidth * glow[0], kVideoHeight * (1 - glow[1]));
        CGContextDrawRadialGradient(context, soft, centre, 0, centre, kVideoWidth * 0.45, 0);
        CGGradientRelease(soft);
    }
    CGImageRef image = CGBitmapContextCreateImage(context);
    CGGradientRelease(gradient);
    CGContextRelease(context);
    CGColorSpaceRelease(space);
    return image;
}

void drawCursor(CGContextRef context, Point at, double alpha, double press)
{
    if (alpha <= 0.0) {
        return;
    }
    CGContextSaveGState(context);
    CGContextSetAlpha(context, alpha);
    if (press > 0.0) {
        // The click: a ring that grows and fades.
        CGColorRef ring = rgba(1, 1, 1, 0.55 * (1 - press));
        CGContextSetStrokeColorWithColor(context, ring);
        CGContextSetLineWidth(context, 3);
        const double radius = 10 + 26 * easeOut(press);
        CGContextStrokeEllipseInRect(context, CGRectMake(at.x - radius, at.y - radius, 2 * radius,
                                                         2 * radius));
        CGColorRelease(ring);
    }
    CGContextTranslateCTM(context, at.x, at.y);
    CGContextScaleCTM(context, 1.5, 1.5);
    CGMutablePathRef arrow = CGPathCreateMutable();
    const CGPoint points[] = {{0, 0}, {0, 17}, {4, 13.2}, {6.8, 19.6}, {9.6, 18.4}, {6.9, 12.2}, {12.2, 12.2}};
    CGPathAddLines(arrow, nullptr, points, 7);
    CGPathCloseSubpath(arrow);
    CGColorRef shadow = rgba(0, 0, 0, 0.45);
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

// A caption: one line under a pill, rising in and fading out.
void drawCaption(CGContextRef context, const std::string& text, double visible)
{
    if (visible <= 0.0) {
        return;
    }
    const Text caption{.text = text, .size = 46, .weight = NSFontWeightBold, .kern = -0.5,
                       .a = visible};
    CTLineRef line = makeLine(caption);
    const double width = lineWidth(line);
    CFRelease(line);
    const double y = 112 - 14 * (1 - visible);
    CGContextSaveGState(context);
    CGPathRef pill = CGPathCreateWithRoundedRect(
        CGRectMake(kVideoWidth / 2.0 - width / 2 - 34, y - 50, width + 68, 72), 36, 36, nullptr);
    fill(context, pill, 0.03, 0.035, 0.05, 0.72 * visible);
    CGColorRef edge = rgba(1, 1, 1, 0.10 * visible);
    CGContextSetStrokeColorWithColor(context, edge);
    CGContextSetLineWidth(context, 1);
    CGContextAddPath(context, pill);
    CGContextStrokePath(context);
    CGColorRelease(edge);
    CGPathRelease(pill);
    CGContextRestoreGState(context);
    drawText(context, caption, kVideoWidth / 2.0, y);
}

void drawLogo(CGContextRef context, double alpha, double y, const std::string& line)
{
    if (alpha <= 0.0) {
        return;
    }
    drawText(context, {.text = "O-SCOPE", .size = 124, .weight = NSFontWeightHeavy, .kern = 22, .a = alpha},
             kVideoWidth / 2.0, y);
    drawText(context, {.text = line, .size = 34, .weight = NSFontWeightMedium, .kern = 1,
                       .r = 0.72, .g = 0.76, .b = 0.84, .a = alpha},
             kVideoWidth / 2.0, y + 70);
}

// The camera: how much the window is magnified and which point of it (in window coordinates:
// CSS pixels, the title bar included) sits where its centre would.
struct Camera
{
    double zoom = 1.0;
    Point focus{kEditorWidth / 2, kWindowHeight / 2};
};

struct Look
{
    double opacity = 1.0;
    double rise = 0.0; // Video pixels the window sits lower by.
    double scale = 1.0;
};

// Window coordinates to video coordinates.
Point toVideo(const Camera& camera, const Look& look, Point p)
{
    const double scale = kWindowScale * camera.zoom * look.scale;
    return {kVideoWidth / 2.0 + (p.x - camera.focus.x) * scale,
            kWindowCentreY + look.rise + (p.y - camera.focus.y) * scale};
}

void drawWindow(CGContextRef context, CGImageRef page, const Camera& camera, const Look& look,
                const std::string& title)
{
    if (look.opacity <= 0.0) {
        return;
    }
    const Point origin = toVideo(camera, look, {0, 0});
    const double scale = kWindowScale * camera.zoom * look.scale;
    const CGRect frame = CGRectMake(origin.x, origin.y, kEditorWidth * scale, kWindowHeight * scale);
    const double radius = 11 * scale;
    CGContextSaveGState(context);
    CGContextSetAlpha(context, look.opacity);
    CGContextBeginTransparencyLayer(context, nullptr);
    CGPathRef outline = CGPathCreateWithRoundedRect(frame, radius, radius, nullptr);
    CGColorRef shadow = rgba(0, 0, 0, 0.6);
    CGContextSetShadowWithColor(context, CGSizeMake(0, 24 * scale), 70 * scale, shadow);
    fill(context, outline, 0.11, 0.11, 0.12, 1);
    CGContextSetShadowWithColor(context, CGSizeZero, 0, nullptr);
    CGColorRelease(shadow);
    CGContextAddPath(context, outline);
    CGContextClip(context);
    // The title bar, as macOS draws a dark window's.
    const CGRect bar = CGRectMake(frame.origin.x, frame.origin.y, frame.size.width, kTitleBar * scale);
    CGPathRef barPath = CGPathCreateWithRect(bar, nullptr);
    fill(context, barPath, 0.16, 0.16, 0.17, 1);
    CGPathRelease(barPath);
    const double lights[][3] = {{1.0, 0.37, 0.34}, {1.0, 0.74, 0.18}, {0.16, 0.78, 0.25}};
    for (int i = 0; i < 3; ++i) {
        const double cx = frame.origin.x + (20 + 20 * i) * scale;
        const double cy = frame.origin.y + kTitleBar / 2 * scale;
        CGPathRef light = CGPathCreateWithEllipseInRect(
            CGRectMake(cx - 6 * scale, cy - 6 * scale, 12 * scale, 12 * scale), nullptr);
        fill(context, light, lights[i][0], lights[i][1], lights[i][2], 1);
        CGPathRelease(light);
    }
    drawText(context, {.text = title, .size = 13 * scale, .weight = NSFontWeightSemibold,
                       .r = 0.78, .g = 0.78, .b = 0.8},
             frame.origin.x + frame.size.width / 2, frame.origin.y + 18.5 * scale);
    if (page != nullptr) {
        drawImage(context, page,
                  CGRectMake(frame.origin.x, frame.origin.y + kTitleBar * scale, frame.size.width,
                             kEditorHeight * scale));
    }
    CGPathRelease(outline);
    CGContextEndTransparencyLayer(context);
    CGContextRestoreGState(context);
    // A hairline edge, as on a real window.
    CGContextSaveGState(context);
    CGPathRef edge = CGPathCreateWithRoundedRect(CGRectInset(frame, 0.5, 0.5), radius, radius, nullptr);
    CGColorRef line = rgba(1, 1, 1, 0.12 * look.opacity);
    CGContextSetStrokeColorWithColor(context, line);
    CGContextSetLineWidth(context, 1);
    CGContextAddPath(context, edge);
    CGContextStrokePath(context);
    CGColorRelease(line);
    CGPathRelease(edge);
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
        video = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo outputSettings:videoSettings];
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
                                           .mFormatFlags = kAudioFormatFlagIsFloat |
                                                           kAudioFormatFlagIsPacked,
                                           .mBytesPerPacket = 8,
                                           .mFramesPerPacket = 1,
                                           .mBytesPerFrame = 8,
                                           .mChannelsPerFrame = 2,
                                           .mBitsPerChannel = 32};
        CMAudioFormatDescriptionCreate(nullptr, &format, 0, nullptr, 0, nullptr, nullptr, &audioFormat);
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
        if (!video.readyForMoreMediaData ||
            ![adaptor appendPixelBuffer:buffer withPresentationTime:CMTimeMake(index, kFps)]) {
            CVPixelBufferRelease(buffer);
            throw std::runtime_error(std::string("video frame refused at ") + std::to_string(index) +
                                     " (audio ready " + std::to_string(audio.readyForMoreMediaData) +
                                     ", status " + std::to_string(writer.status) + "): " +
                                     (writer.error ? writer.error.description.UTF8String : "not ready"));
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
        if (sample == nullptr || !audio.readyForMoreMediaData || ![audio appendSampleBuffer:sample]) {
            throw std::runtime_error(std::string("audio refused: ") +
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
            std::fprintf(stderr, "encoding failed: %s\n", writer.error.localizedDescription.UTF8String);
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

        const Groove groove = makeGroove();
        struct Member
        {
            const char* name;
            std::uint32_t colour;
            const Signal* signal;
        };
        const std::vector<Member> members = {
            {"Kick In", 0xE8484A, &groove.kickIn}, {"Clap", 0xEE7D32, &groove.clap},
            {"Hats", 0xF2D64B, &groove.hats},      {"Bass", 0x3D8AD6, &groove.bass},
            {"Keys", 0x5BBE3C, &groove.keys},      {"Kick Out", 0xB06EE6, &groove.kickOut},
        };
        std::vector<std::shared_ptr<engine::Engine>> engines;
        for (std::size_t i = 0; i < members.size(); ++i) {
            auto engine = std::make_shared<engine::Engine>();
            engine->setSampleRate(kRate);
            engine->setInputChannels(1);
            engine->setTrackInfo({.name = members[i].name,
                                  .colour = members[i].colour,
                                  .index = static_cast<int>(i)});
            engine::InstanceRegistry::shared().add(engine);
            engines.push_back(engine);
        }
        engine::Engine& own = *engines.front();
        using protocol::SettingId;
        set(own, SettingId::kDisplayMode, "single");
        set(own, SettingId::kTimeBase, "beats");
        set(own, SettingId::kBeatSpan, "oneBar");
        set(own, SettingId::kViewLayout, "stacked");
        set(*engines.back(), SettingId::kGroup, "b"); // Kick Out joins for Compare.

        // The window goes on the sharpest screen, so the page is filmed at its backing scale.
        NSScreen* screen = NSScreen.mainScreen;
        for (NSScreen* candidate in NSScreen.screens) {
            if (candidate.backingScaleFactor > screen.backingScaleFactor) {
                screen = candidate;
            }
        }
        const NSRect visible = screen.visibleFrame;
        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(visible.origin.x + 40,
                                           visible.origin.y + visible.size.height - kEditorHeight - 60,
                                           kEditorWidth, kEditorHeight)
                      styleMask:NSWindowStyleMaskTitled
                        backing:NSBackingStoreBuffered
                          defer:NO
                         screen:screen];
        window.releasedWhenClosed = NO;
        window.title = @"O-Scope showcase (filming)";
        NSView* content = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, kEditorWidth, kEditorHeight)];
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
        waitUntil([&] { return evaluate(webView, "String(typeof window.orizonDebug)") == "object"; }, 10.0);
        evaluate(webView, kPageHelpers);

        // The film's state, moved by the script.
        double now = 0.0;
        Track<double> zoom(1.0);
        Track<Point> focus(Point{kEditorWidth / 2, kWindowHeight / 2});
        Track<Point> cursor(Point{kEditorWidth * 0.55, kWindowHeight * 0.62});
        double cursorShown = -1.0; // When the cursor appears; it fades in.
        double pressAt = -10.0;
        bool live = true;      // Frames still arrive (not frozen).
        bool hovering = false; // The pointer is over the traces, so the page hears it move.
        double zoomUntil = -1.0;
        double zoomRate = 0.0; // Wheel delta per frame while zooming.

        const auto centre = [&](const std::string& selector, const std::string& text = "") {
            const std::string args = text.empty() ? "'" + selector + "'" : "'" + selector + "','" + text + "'";
            const std::string found = evaluate(webView, "window.showcase.centre(" + args + ")");
            const auto json = nlohmann::json::parse(found.empty() ? "[600,340]" : found, nullptr, false);
            return Point{json.is_array() ? json[0].get<double>() : 600.0,
                         (json.is_array() ? json[1].get<double>() : 340.0) + kTitleBar};
        };
        const auto pageCall = [&](const std::string& call) { evaluate(webView, "window.showcase." + call); };
        const auto moveTo = [&](Point target, double duration) { cursor.move(now, target, duration); };
        const auto click = [&](const std::string& selector, const std::string& text = "") {
            pressAt = now;
            const std::string args = text.empty() ? "'" + selector + "'" : "'" + selector + "','" + text + "'";
            pageCall("click(" + args + ")");
        };
        const auto look = [&](double magnify, Point at, double duration) {
            zoom.move(now, magnify, duration);
            focus.move(now, at, duration);
        };
        const auto group = [&](std::size_t index, const char* name) {
            set(*engines[index], SettingId::kGroup, name);
        };

        // The script: when, and what.
        struct Cue
        {
            double at;
            std::function<void()> run;
        };
        Point kickHit{};
        std::vector<Cue> cues = {
            {5.6, [&] { cursorShown = now; }},
            {5.8, [&] { moveTo(centre("#display-mode button", "multi"), 0.8); }},
            {6.7, [&] { click("#display-mode button", "multi"); }},
            {8.6, [&] { look(1.06, {kEditorWidth / 2, kWindowHeight / 2 + 10}, 2.6); }},
            {10.9, [&] { moveTo(centre("#freeze"), 0.8); }},
            {11.9, [&] { click("#freeze"); live = false; }},
            {12.3, [&] {
                // The attack of the frozen bar's first kick, at the start of the traces.
                const auto lane = centre("#lane-labels span", "kick in");
                const std::string left = evaluate(webView, "String(document.getElementById('trace').getBoundingClientRect().left)");
                kickHit = {(left.empty() ? 32.0 : std::stod(left)) + 3, lane.y + 40};
                moveTo(kickHit, 0.7);
                look(2.1, {kickHit.x + 200, kickHit.y}, 1.0);
            }},
            {13.2, [&] { zoomUntil = 15.4; zoomRate = -2.6; hovering = false; }},
            {15.8, [&] { look(1.0, {kEditorWidth / 2, kWindowHeight / 2}, 0.8); }},
            {16.1, [&] { pageCall("reset()"); }},
            {16.4, [&] { moveTo(centre("#hover-toggle"), 0.7); }},
            {17.2, [&] { click("#hover-toggle"); }},
            {17.5, [&] {
                const auto bass = centre("#lane-labels span", "bass");
                moveTo({kEditorWidth * 0.30, bass.y + 46}, 0.7);
                look(1.25, {kEditorWidth * 0.45, bass.y}, 1.0);
            }},
            {18.2, [&] { hovering = true; moveTo({kEditorWidth * 0.62, cursor.target().y - 60}, 2.2); }},
            {20.5, [&] { hovering = false; pageCall("leave()"); }},
            {20.6, [&] {
                pageCall("click('#hover-toggle')");
                pageCall("click('#freeze')");
                live = true;
                group(1, "b"), group(2, "b"), group(3, "b"), group(4, "b"), group(5, "a");
                look(1.0, {kEditorWidth / 2, kWindowHeight / 2}, 0.9);
                moveTo(centre("#display-mode button", "compare"), 0.8);
            }},
            {21.5, [&] { click("#display-mode button", "compare"); }},
            {22.1, [&] {
                // Kick Out is compared already: point at how it lines up, then at the sum.
                const auto out = centre("#lane-labels span", "kick out");
                moveTo({out.x + 60, out.y + 2}, 0.8);
                look(1.45, {kEditorWidth * 0.36, kWindowHeight * 0.45}, 1.2);
            }},
            {24.0, [&] { moveTo(centre(".preview-fix"), 0.7); }},
            {24.8, [&] { click(".preview-fix"); }},
            {26.3, [&] { look(1.0, {kEditorWidth / 2, kWindowHeight / 2}, 0.9); }},
        };
        std::size_t nextCue = 0;

        struct Caption
        {
            double from;
            double to;
            const char* text;
        };
        const Caption captions[] = {
            {3.0, 6.3, "A real-time oscilloscope for your DAW"},
            {7.0, 10.7, "Every track. Lined up on the beat."},
            {12.2, 16.1, "Freeze. Zoom down to the sample."},
            {17.4, 20.6, "Read time and level anywhere."},
            {21.8, 26.3, "Spot phase problems. Preview the fix."},
        };

        Encoder encoder(options.out);
        CGImageRef background = makeBackground();
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        std::uint64_t sample = 0;
        double framesDrawn = 0.0;
        const auto drawn = [&] {
            const std::string text = evaluate(webView, "String(window.orizonDebug.perf.framesDrawn)");
            return text.empty() ? 0.0 : std::stod(text);
        };
        // One frame of audio into every engine, on one host clock.
        const auto play = [&] {
            std::vector<float> left(kBlock);
            for (std::size_t track = 0; track < engines.size(); ++track) {
                const Signal& signal = *members[track].signal;
                for (int i = 0; i < kBlock; ++i) {
                    left[static_cast<std::size_t>(i)] = signal[(sample + static_cast<std::uint64_t>(i)) % signal.size()];
                }
                std::array<const float*, 2> channels{left.data(), left.data()};
                engines[track]->captureRing().write(channels.data(), 2, kBlock);
                const double seconds = static_cast<double>(sample) / kRate;
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
            evaluate(webView, "window.showcase.now = " + std::to_string(pageTime * 1000.0) + "; ''");
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

        // The soundtrack for video frame `index`: what the tracks play during it, mixed, faded in
        // and out.
        const auto prerollSamples = static_cast<std::uint64_t>(kPreroll * kRate);
        int soundFrames = 0;
        const auto appendSound = [&](int index) {
            std::vector<float> mix(2 * kBlock);
            const std::uint64_t first = prerollSamples + static_cast<std::uint64_t>(index) * kBlock;
            for (int i = 0; i < kBlock; ++i) {
                const double t = static_cast<double>(index) / kFps + static_cast<double>(i) / kRate;
                const double fade = std::min(clamp01(t / 0.3), clamp01((options.seconds - t) / 1.5));
                const auto at = (first + static_cast<std::uint64_t>(i)) % groove.kickIn.size();
                const double centre = 0.34 * groove.kickIn[at] + 0.26 * groove.clap[at] + 0.30 * groove.bass[at];
                const double hats = 0.18 * groove.hats[at];
                const double keys = 0.26 * groove.keys[at];
                mix[2 * static_cast<std::size_t>(i)] = static_cast<float>(fade * (centre + 0.7 * hats + keys));
                mix[2 * static_cast<std::size_t>(i) + 1] = static_cast<float>(fade * (centre + hats + 0.7 * keys));
            }
            encoder.appendAudio(mix, static_cast<std::int64_t>(index) * kBlock);
        };

        const int total = options.frames > 0 ? options.frames : static_cast<int>(options.seconds * kFps);
        const auto started = std::chrono::steady_clock::now();
        for (int f = 0; f < total; ++f) {
            now = static_cast<double>(f) / kFps;
            step(kPreroll + now);
            while (nextCue < cues.size() && cues[nextCue].at <= now + 1e-9) {
                cues[nextCue++].run();
            }
            const Point pointer = cursor.at(now);
            if (now < zoomUntil) {
                pageCall("zoom(" + std::to_string(pointer.x) + "," + std::to_string(pointer.y - kTitleBar) +
                         "," + std::to_string(zoomRate) + ")");
            }
            if (hovering) {
                pageCall("hover(" + std::to_string(pointer.x) + "," + std::to_string(pointer.y - kTitleBar) + ")");
            }
            settle(webView);
            CGImageRef page = snapshot(webView);
            if (f == 0 && page != nullptr) {
                std::printf("page filmed at %zux%zu\n", CGImageGetWidth(page), CGImageGetHeight(page));
            }

            CVPixelBufferRef buffer = encoder.frame();
            CVPixelBufferLockBaseAddress(buffer, 0);
            CGContextRef context = CGBitmapContextCreate(
                CVPixelBufferGetBaseAddress(buffer), kVideoWidth, kVideoHeight, 8,
                CVPixelBufferGetBytesPerRow(buffer), space,
                static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedFirst) | kCGBitmapByteOrder32Little);
            CGContextTranslateCTM(context, 0, kVideoHeight);
            CGContextScaleCTM(context, 1, -1);
            drawImage(context, background, CGRectMake(0, 0, kVideoWidth, kVideoHeight));

            // Intro: the name, then the window rises in. Outro: the window gives way to the name.
            const double intro = smooth((now - 2.0) / 0.9);
            const double outro = smooth((now - 26.9) / 0.8);
            drawLogo(context, shown(now, 0.2, 2.6, 0.6, 0.5), 560 - 40 * smooth((now - 2.0) / 0.6),
                     "by Orizon Audio");
            Look appearance{.opacity = intro * (1 - outro),
                            .rise = 70 * (1 - easeOut((now - 2.0) / 0.9)) + 40 * outro,
                            .scale = 1 - 0.05 * (1 - intro) - 0.04 * outro};
            const Camera view{.zoom = zoom.at(now), .focus = focus.at(now)};
            drawWindow(context, page, view, appearance, "O-Scope/Kick In");
            for (const Caption& caption : captions) {
                drawCaption(context, caption.text, shown(now, caption.from, caption.to));
            }
            const double cursorAlpha =
                cursorShown < 0.0 ? 0.0 : smooth((now - cursorShown) / 0.3) * (1 - smooth((now - 26.4) / 0.4));
            const double press = now - pressAt < 0.35 ? (now - pressAt) / 0.35 : 0.0;
            drawCursor(context, toVideo(view, appearance, pointer), cursorAlpha, press);
            drawLogo(context, smooth((now - 27.5) / 0.7), 500, "Free and open source  ·  VST3  ·  macOS and Windows");
            // Fade to black at the very end.
            const double black = smooth((now - (options.seconds - 0.6)) / 0.6);
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
                const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                std::printf("%5.1f s of %.0f filmed (%.0f s so far)\n", now, options.seconds, elapsed);
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
        return run(parse(argc, argv));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "showcase: %s\n", error.what());
        return 1;
    }
}
