// Performance benchmark: the real editor page in a real WKWebView, showing tracks that play in
// real time, as in a host. Reports the CPU of this process (by thread) and of the WebKit
// processes it starts, the frames the page receives and draws, and the page's time per frame.
//
//   perf_bench [--lanes=7] [--mode=multi|compare|single] [--timebase=beats|seconds]
//              [--beatspan=fourBars] [--span=4] [--layout=stacked|overlaid]
//              [--width=1400] [--height=800] [--warmup=3] [--duration=10]
#include "editor/Editor.h"
#include "engine/Engine.h"
#include "engine/InstanceRegistry.h"
#include "engine/SettingJson.h"
#include "pluginterfaces/gui/iplugview.h"

#import <Cocoa/Cocoa.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#import <WebKit/WebKit.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <map>
#include <memory>
#include <numbers>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <vector>

using namespace orizon;
using namespace Steinberg;

namespace {

struct Options
{
    int lanes = 7;
    std::string mode = "multi";
    std::string timeBase = "beats";
    std::string beatSpan = "fourBars";
    double span = 4.0;
    std::string layout = "stacked";
    int width = 1400;
    int height = 800;
    double warmup = 3.0;
    double duration = 10.0;
    bool idle = false; // No audio: the page open with nothing to draw.
};

Options parse(std::span<const char* const> arguments)
{
    Options options;
    for (const char* const text : arguments.subspan(1)) {
        const std::string argument = text;
        const auto equals = argument.find('=');
        const std::string key = argument.substr(0, equals);
        const std::string value = equals == std::string::npos ? "" : argument.substr(equals + 1);
        if (key == "--lanes") {
            options.lanes = std::stoi(value);
        } else if (key == "--mode") {
            options.mode = value;
        } else if (key == "--timebase") {
            options.timeBase = value;
        } else if (key == "--beatspan") {
            options.beatSpan = value;
        } else if (key == "--span") {
            options.span = std::stod(value);
        } else if (key == "--layout") {
            options.layout = value;
        } else if (key == "--width") {
            options.width = std::stoi(value);
        } else if (key == "--height") {
            options.height = std::stoi(value);
        } else if (key == "--warmup") {
            options.warmup = std::stod(value);
        } else if (key == "--duration") {
            options.duration = std::stod(value);
        } else if (key == "--idle") {
            options.idle = true;
        }
    }
    return options;
}

constexpr double kRate = 48000.0;
constexpr int kBlock = 480; // 10 ms, as a host block.
constexpr double kTempo = 120.0;

// Track n's signal at host time `t` (seconds): kicks, bass, hats, noise beds and tones.
float signalOf(int track, double t)
{
    const double beat = 60.0 / kTempo;
    const double inBeat = std::fmod(t, beat);
    const double pi = std::numbers::pi;
    switch (track % 5) {
    case 0:
        return static_cast<float>(
            0.9 * std::exp(-inBeat * 9.0) *
            std::sin(2.0 * pi * (50.0 + 100.0 * std::exp(-inBeat / 0.03)) * inBeat));
    case 1:
        return static_cast<float>(0.45 * std::sin(2.0 * pi * 55.0 * t));
    case 2: {
        const double inEighth = std::fmod(t, beat / 2);
        const auto noise = static_cast<double>(std::rand()) / RAND_MAX - 0.5;
        return static_cast<float>(0.4 * std::exp(-inEighth * 40.0) * noise);
    }
    case 3:
        return static_cast<float>(0.3 * std::sin(2.0 * pi * 220.0 * t) +
                                  0.1 * std::sin(2.0 * pi * 660.0 * t));
    default:
        return static_cast<float>(0.2 * (static_cast<double>(std::rand()) / RAND_MAX - 0.5));
    }
}

// Plays every track in real time: one block per engine every 10 ms, on one host clock.
class Host
{
public:
    explicit Host(std::vector<std::shared_ptr<engine::Engine>> engines)
        : engines(std::move(engines)), thread([this] { run(); })
    {}
    ~Host()
    {
        stopping = true;
        thread.join();
    }
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

private:
    void run()
    {
        pthread_setname_np("bench host");
        std::vector<float> left(kBlock);
        std::vector<float> right(kBlock);
        std::uint64_t sample = 0;
        auto next = std::chrono::steady_clock::now();
        while (!stopping) {
            for (std::size_t track = 0; track < engines.size(); ++track) {
                for (int i = 0; i < kBlock; ++i) {
                    const double t =
                        static_cast<double>(sample + static_cast<std::uint64_t>(i)) / kRate;
                    left[static_cast<std::size_t>(i)] = signalOf(static_cast<int>(track), t);
                    right[static_cast<std::size_t>(i)] = 0.8f * left[static_cast<std::size_t>(i)];
                }
                std::array<const float*, 2> channels{left.data(), right.data()};
                engine::Engine& engine = *engines[track];
                engine.captureRing().write(channels.data(), 2, kBlock);
                const double seconds = static_cast<double>(sample) / kRate;
                engine.timeline().advance({.playing = true,
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
            sample += kBlock;
            next += std::chrono::microseconds(10000);
            std::this_thread::sleep_until(next);
        }
    }

    std::vector<std::shared_ptr<engine::Engine>> engines;
    std::atomic<bool> stopping{false};
    std::thread thread;
};

void pump(double seconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        // Window server events too, so the window is shown and the page counts as visible.
        NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
        if (event != nil) {
            [NSApp sendEvent:event];
        }
    }
}

std::string evaluate(WKWebView* webView, NSString* script)
{
    __block std::string result;
    __block bool done = false;
    [webView evaluateJavaScript:script
              completionHandler:^(id value, NSError* error) {
                (void)error;
                if ([value isKindOfClass:[NSString class]]) {
                    result = [static_cast<NSString*>(value) UTF8String];
                }
                done = true;
              }];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done && std::chrono::steady_clock::now() < deadline) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
    }
    return result;
}

// CPU seconds of each thread of this process, by name (unnamed threads are summed as "other").
std::map<std::string, double> threadTimes()
{
    std::map<std::string, double> times;
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
        return times;
    }
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        thread_extended_info_data_t info{};
        mach_msg_type_number_t infoCount = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(threads[i], THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&info),
                        &infoCount) == KERN_SUCCESS) {
            std::string name = info.pth_name;
            if (i == 0) {
                name = "main";
            } else if (name.empty()) {
                name = "other";
            }
            times[name] += static_cast<double>(info.pth_user_time + info.pth_system_time) / 1e9;
        }
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads),
                  count * sizeof(thread_act_t));
    return times;
}

// CPU seconds of every WebKit process, by pid, with its name.
std::map<int, std::pair<std::string, double>> webKitTimes()
{
    std::map<int, std::pair<std::string, double>> times;
    std::vector<pid_t> pids(4096);
    const int bytes = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    if (bytes <= 0) {
        return times;
    }
    mach_timebase_info_data_t timebase{};
    mach_timebase_info(&timebase);
    pids.resize(static_cast<std::size_t>(bytes) / sizeof(pid_t));
    std::array<char, PROC_PIDPATHINFO_MAXSIZE> path{};
    for (const pid_t pid : pids) {
        if (proc_pidpath(pid, path.data(), static_cast<std::uint32_t>(path.size())) <= 0) {
            continue;
        }
        const std::string name = path.data();
        if (name.find("com.apple.WebKit") == std::string::npos) {
            continue;
        }
        rusage_info_v2 usage{};
        if (proc_pid_rusage(pid, RUSAGE_INFO_V2, reinterpret_cast<rusage_info_t*>(&usage)) != 0) {
            continue;
        }
        const auto ticks = static_cast<double>(usage.ri_user_time + usage.ri_system_time);
        times[pid] = {name.substr(name.rfind('/') + 1),
                      ticks * timebase.numer / timebase.denom / 1e9};
    }
    return times;
}

double processTime()
{
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
           static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
}

double residentMegabytes()
{
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count);
    return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
}

void set(engine::Engine& engine, protocol::SettingId id, const char* value)
{
    if (const auto parsed = engine::settingFromJson(id, nlohmann::json(value))) {
        engine.settings().set(id, *parsed);
    }
}

int run(const Options& options)
{
    @autoreleasepool {
        // A sleeping display makes compositing nearly free; keep it awake, so runs compare.
        IOPMAssertionID displayAwake = 0;
        IOPMAssertionDeclareUserActivity(CFSTR("O-Scope benchmark"), kIOPMUserActiveLocal,
                                         &displayAwake);
        IOPMAssertionID noSleep = 0;
        IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep,
                                    kIOPMAssertionLevelOn, CFSTR("O-Scope benchmark"), &noSleep);
        [NSApplication sharedApplication];
        // A foreground app with a visible window, or WebKit throttles the page's draw clock.
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp finishLaunching];

        std::vector<std::shared_ptr<engine::Engine>> engines;
        const int tracks = options.mode == "single" ? 1 : options.lanes;
        for (int i = 0; i < tracks; ++i) {
            auto engine = std::make_shared<engine::Engine>();
            engine->setSampleRate(kRate);
            engine->setTrackInfo(
                {.name = "Track " + std::to_string(i + 1), .colour = std::nullopt, .index = i});
            engine::InstanceRegistry::shared().add(engine);
            engines.push_back(engine);
        }
        engine::Engine& own = *engines.front();
        using protocol::SettingId;
        set(own, SettingId::kDisplayMode, options.mode.c_str());
        set(own, SettingId::kTimeBase, options.timeBase.c_str());
        set(own, SettingId::kBeatSpan, options.beatSpan.c_str());
        set(own, SettingId::kViewLayout, options.layout.c_str());
        set(own, SettingId::kTriggerMode, "free");
        own.settings().set(SettingId::kTimeSpan, options.span);
        if (options.mode == "single") {
            own.settings().set(SettingId::kChannelViews, 15.0); // L, R, M and S.
        }
        const std::unique_ptr<Host> host = options.idle ? nullptr : std::make_unique<Host>(engines);

        const auto before = webKitTimes();
        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, options.width, options.height)
                                        styleMask:NSWindowStyleMaskTitled
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        window.releasedWhenClosed = NO;
        NSView* content =
            [[NSView alloc] initWithFrame:NSMakeRect(0, 0, options.width, options.height)];
        window.contentView = content;
        window.level = NSFloatingWindowLevel;
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        IPtr<editor::Editor> editor = owned(new editor::Editor(engines.front()));
        ViewRect rect(0, 0, options.width, options.height);
        editor->onSize(&rect);
        editor->attached((__bridge void*)content, kPlatformTypeNSView);
        auto* webView = (__bridge WKWebView*)editor->webView()->nativeView();

        pump(options.warmup);
        const std::string perfScript = "JSON.stringify(window.orizonDebug?.perf ?? "
                                       "{frameCount: window.orizonDebug?.frameCount ?? 0})";
        auto readPerf = [&] {
            const std::string text = evaluate(webView, @(perfScript.c_str()));
            return nlohmann::json::parse(text.empty() ? "{}" : text, nullptr, false);
        };

        std::printf("page state: %s\n",
                    evaluate(webView, @"JSON.stringify({visibility: document.visibilityState, "
                                      @"width: innerWidth, height: innerHeight, "
                                      @"frames: window.orizonDebug?.frameCount, "
                                      @"perf: window.orizonDebug?.perf})")
                        .c_str());
        const auto startPerf = readPerf();
        const auto startThreads = threadTimes();
        const double startProcess = processTime();
        const auto startWebKit = webKitTimes();
        const auto startWall = std::chrono::steady_clock::now();
        pump(options.duration);
        const double wall =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - startWall).count();
        const auto endWebKit = webKitTimes();
        const double endProcess = processTime();
        const auto endThreads = threadTimes();
        const auto endPerf = readPerf();

        std::printf("config: %s, %d track(s), %s, %s, %s, %dx%d, Debug=%d\n", options.mode.c_str(),
                    tracks, options.timeBase.c_str(),
                    options.timeBase == "beats" ? options.beatSpan.c_str()
                                                : (std::to_string(options.span) + " s").c_str(),
                    options.layout.c_str(), options.width, options.height,
#ifdef NDEBUG
                    0
#else
                    1
#endif
        );
        std::printf("plugin process: %.1f%% of a core, %.0f MB resident\n",
                    100.0 * (endProcess - startProcess) / wall, residentMegabytes());
        for (const auto& [name, end] : endThreads) {
            const auto found = startThreads.find(name);
            const double used = end - (found == startThreads.end() ? 0.0 : found->second);
            if (used / wall >= 0.005) {
                std::printf("  thread %-18s %5.1f%%\n", name.c_str(), 100.0 * used / wall);
            }
        }
        double webKitTotal = 0.0;
        for (const auto& [pid, entry] : endWebKit) {
            if (before.contains(pid)) {
                continue; // Another app's WebKit process.
            }
            const auto found = startWebKit.find(pid);
            const double used =
                entry.second - (found == startWebKit.end() ? 0.0 : found->second.second);
            webKitTotal += used;
            std::printf("  %-36s %5.1f%%\n", entry.first.c_str(), 100.0 * used / wall);
        }
        std::printf("webkit processes: %.1f%% of a core\n", 100.0 * webKitTotal / wall);
        std::printf("total: %.1f%% of a core\n",
                    100.0 * (endProcess - startProcess + webKitTotal) / wall);

        const auto delta = [&](const char* key) {
            return endPerf.value(key, 0.0) - startPerf.value(key, 0.0);
        };
        // Builds without the page's counters still count frames.
        const double received =
            startPerf.contains("framesReceived") ? delta("framesReceived") : delta("frameCount");
        const double drawn = delta("framesDrawn");
        std::printf("page: %.1f frames/s received, %.1f drawn, %.0f KB/frame\n", received / wall,
                    drawn / wall, received > 0 ? delta("bytesReceived") / received / 1024.0 : 0.0);
        std::printf("page: receive %.2f ms/frame, draw %.2f ms/frame (max %.1f ms overall)\n",
                    received > 0 ? delta("receiveMs") / received : 0.0,
                    drawn > 0 ? delta("drawMs") / drawn : 0.0, endPerf.value("maxDrawMs", 0.0));

        editor->removed();
        [window close];
        IOPMAssertionRelease(noSleep);
        IOPMAssertionRelease(displayAwake);
    }
    return 0;
}

} // namespace

int main(int argc, const char* argv[])
{
    try {
        return run(parse(std::span(argv, static_cast<std::size_t>(argc))));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "perf_bench: %s\n", error.what());
        return 1;
    }
}
