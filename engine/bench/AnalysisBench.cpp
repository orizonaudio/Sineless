// The engine's work per frame, for one scenario (bench/Scenarios.h): the editor's analysis, with
// no editor, page or audio thread, on audio played in steps, so that two runs of one build do
// exactly the same work. Run under Valgrind's Cachegrind (scripts/engine-work.sh), it counts the
// instructions of the measured frames alone; run on its own, it times them.
//
//   analysis_bench --list
//   analysis_bench --scenario=NAME [--columns=2560] [--frames=300] [--json=PATH]
//
// The scenario's sweep or window is filled first, a frame a second, then settled for a second at
// 60 frames a second; then `frames` frames are measured, one per 1/60 s of audio, as a page asks
// for them. Prints and writes: the frames that showed something new, the columns the engine drew
// (from the frames, not from --columns), the work counted per frame (AnalysisWork) and, run on
// its own, the time per frame at the median and the 99th percentile.
#include "bench/Scenarios.h"
#include "bench/Signals.h"
#include "engine/Analyzer.h"
#include "engine/Engine.h"
#include "engine/InstanceRegistry.h"
#include "engine/Sweep.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if __has_include(<valgrind/cachegrind.h>)
#include <valgrind/cachegrind.h>
#include <valgrind/valgrind.h>
#define ORIZON_COUNT_START() CACHEGRIND_START_INSTRUMENTATION
#define ORIZON_COUNT_STOP() CACHEGRIND_STOP_INSTRUMENTATION
#define ORIZON_UNDER_VALGRIND() (RUNNING_ON_VALGRIND != 0)
#else
#define ORIZON_COUNT_START()
#define ORIZON_COUNT_STOP()
#define ORIZON_UNDER_VALGRIND() false
#endif

using namespace orizon;
using protocol::SettingId;

namespace {

struct Options
{
    std::string scenario;
    // A 1280-pixel-wide screen at twice its pixel density: a Retina Mac's, the heavier case.
    std::size_t columns = 2560;
    int frames = 300; // Five seconds at 60 frames a second.
    std::string json;
    bool list = false;
};

Options parse(std::span<char* const> arguments)
{
    Options options;
    for (const std::string_view argument : arguments.subspan(1)) {
        const auto equals = argument.find('=');
        const std::string_view key = argument.substr(0, equals);
        const std::string value(equals == std::string_view::npos ? std::string_view{}
                                                                 : argument.substr(equals + 1));
        if (key == "--scenario") {
            options.scenario = value;
        } else if (key == "--columns") {
            options.columns = std::stoul(value);
        } else if (key == "--frames") {
            options.frames = std::stoi(value);
        } else if (key == "--json") {
            options.json = value;
        } else if (key == "--list") {
            options.list = true;
        }
    }
    return options;
}

// Every track of the scenario, played in host blocks on one clock, as a host plays them.
class Host
{
public:
    explicit Host(const bench::Scenario& scenario)
        : scenario(scenario),
          signals(static_cast<std::size_t>(scenario.tracks), scenario.rate, scenario.tempo),
          left(kBlock), right(kBlock)
    {
        for (int track = 0; track < scenario.tracks; ++track) {
            auto engine = std::make_shared<engine::Engine>();
            engine->prepare(scenario.rate);
            engine->setInputChannels(scenario.channels);
            registry.add(engine);
            engines.push_back(engine);
        }
    }

    engine::Engine& own() { return *engines.front(); }

    // Plays until host sample `end`.
    void playTo(std::uint64_t end)
    {
        while (position < end) {
            const auto count =
                static_cast<std::size_t>(std::min<std::uint64_t>(kBlock, end - position));
            const double seconds = static_cast<double>(position) / scenario.rate;
            for (std::size_t track = 0; track < engines.size(); ++track) {
                signals.fill(track, position, scenario.jitter, std::span(left).first(count),
                             std::span(right).first(count));
                const std::array<const float*, 2> channels{left.data(), right.data()};
                engine::Engine& engine = *engines[track];
                engine.captureRing().write(channels.data(), scenario.channels,
                                           static_cast<int>(count));
                engine.timeline().advance({.playing = true,
                                           .positionValid = true,
                                           .quarterNotes = seconds * scenario.tempo / 60.0,
                                           .tempoValid = true,
                                           .tempo = scenario.tempo,
                                           .timeSignatureValid = true,
                                           .numerator = 4,
                                           .denominator = 4,
                                           .secondsValid = true,
                                           .seconds = seconds},
                                          static_cast<int>(count), scenario.rate);
            }
            position += count;
        }
    }

    [[nodiscard]] std::uint64_t now() const { return position; }

    engine::InstanceRegistry registry;

private:
    static constexpr std::size_t kBlock = 512;

    const bench::Scenario& scenario;
    bench::Signals signals;
    std::vector<std::shared_ptr<engine::Engine>> engines;
    std::vector<float> left;
    std::vector<float> right;
    std::uint64_t position = 0;
};

void configure(engine::Engine& engine, const bench::Scenario& scenario)
{
    engine::SettingsStore& settings = engine.settings();
    settings.set(SettingId::kDisplayMode, static_cast<double>(scenario.mode));
    settings.set(SettingId::kTimeBase, static_cast<double>(scenario.timeBase));
    settings.set(SettingId::kBeatSpan, static_cast<double>(scenario.beatSpan));
    settings.set(SettingId::kTimeSpan, scenario.span);
    settings.set(SettingId::kViewLayout, static_cast<double>(protocol::ViewLayout::kStacked));
    settings.set(SettingId::kChannelViews, 0b1111); // L, R, Mid and Side, in Single.
}

// The seconds the scenario's sweep or window covers.
double spanSeconds(const bench::Scenario& scenario)
{
    if (scenario.timeBase == protocol::TimeBase::kSeconds) {
        return scenario.span;
    }
    return engine::beatSpanQuarterNotes(scenario.beatSpan, 4.0) * 60.0 / scenario.tempo;
}

double percentile(std::vector<double> values, double fraction)
{
    if (values.empty()) {
        return 0.0;
    }
    std::ranges::sort(values);
    const auto index =
        static_cast<std::size_t>(std::lround(fraction * static_cast<double>(values.size() - 1)));
    return values[index];
}

std::uint32_t pointCountOf(const std::vector<std::byte>& frame)
{
    std::uint32_t count = 0;
    if (frame.size() >= protocol::FrameHeader::kSize) {
        std::memcpy(&count, frame.data() + protocol::FrameHeader::kPointCountOffset, sizeof count);
    }
    return count;
}

int run(const bench::Scenario& scenario, const Options& options)
{
    Host host(scenario);
    configure(host.own(), scenario);
    engine::Analyzer analyzer(host.own(), host.registry);
    analyzer.setColumns(options.columns);
    analyzer.setView(scenario.viewStart, scenario.viewEnd);
    if (scenario.mode == protocol::DisplayMode::kCompare) {
        std::vector<std::uint64_t> others;
        for (const engine::GroupMember& member : host.registry.members(protocol::Group::kA)) {
            if (member.engine.get() != &host.own()) {
                others.push_back(member.serial);
                if (!scenario.compareAll) {
                    break;
                }
            }
        }
        analyzer.setCompared(others);
        analyzer.setPreviewFix(scenario.previewFix);
    }

    std::vector<std::byte> frame;
    const auto sampleAt = [&](double seconds) {
        return static_cast<std::uint64_t>(std::llround(seconds * scenario.rate));
    };
    // Fill: a frame a second, which places the audio as frames at 60 a second would.
    const auto fillSeconds = static_cast<int>(std::ceil(spanSeconds(scenario) + 1.0));
    for (int second = 1; second <= fillSeconds; ++second) {
        host.playTo(sampleAt(second));
        (void)analyzer.produce(frame);
    }
    // Settle at 60 frames a second, then freeze if the scenario pans.
    const double frameSeconds = 1.0 / 60.0;
    const std::uint64_t settled = host.now();
    for (int index = 1; index <= 60; ++index) {
        host.playTo(settled + sampleAt(index * frameSeconds));
        (void)analyzer.produce(frame);
    }
    if (scenario.frozenPan) {
        analyzer.setFrozen(true);
        for (int index = 0; index < 64 && analyzer.produce(frame); ++index) {
        }
    }

    const engine::AnalysisWork before = analyzer.work();
    std::vector<double> milliseconds;
    milliseconds.reserve(static_cast<std::size_t>(options.frames));
    std::uint32_t columns = 0;
    int shown = 0;
    const std::uint64_t start = host.now();
    ORIZON_COUNT_START();
    for (int index = 1; index <= options.frames; ++index) {
        host.playTo(start + sampleAt(index * frameSeconds));
        if (scenario.frozenPan) {
            // Back and forth over the first second of what the frozen view can pan through, in 1.5
            // s.
            const double phase = std::fmod(index * frameSeconds / 1.5, 1.0);
            analyzer.setPan(1.0 - std::abs((2.0 * phase) - 1.0));
        }
        const auto began = std::chrono::steady_clock::now();
        const bool produced = analyzer.produce(frame);
        const auto ended = std::chrono::steady_clock::now();
        milliseconds.push_back(std::chrono::duration<double, std::milli>(ended - began).count());
        if (produced) {
            ++shown;
            columns = pointCountOf(frame);
        }
    }
    ORIZON_COUNT_STOP();
    const engine::AnalysisWork& after = analyzer.work();

    const double frames = options.frames;
    const auto perFrame = [&](std::uint64_t from, std::uint64_t to) {
        return static_cast<double>(to - from) / frames;
    };
    const double read = perFrame(before.samplesRead, after.samplesRead);
    const double reduced = perFrame(before.samplesReduced, after.samplesReduced);
    const double summed = perFrame(before.samplesSummed, after.samplesSummed);
    const double searched = perFrame(before.samplesSearched, after.samplesSearched);
    const double searches = perFrame(before.matchSearches, after.matchSearches);
    const bool timed = !ORIZON_UNDER_VALGRIND();
    const double median = percentile(milliseconds, 0.5);
    const double slow = percentile(milliseconds, 0.99);

    std::printf("scenario: %s (%s)\n", std::string(scenario.name).c_str(),
                std::string(scenario.description).c_str());
    std::printf("frames: %d measured, %d showed something new, %u columns\n", options.frames, shown,
                columns);
    std::printf("per frame: %.0f samples read, %.0f reduced, %.0f summed, %.0f searched in "
                "%.3f match searches\n",
                read, reduced, summed, searched, searches);
    if (timed) {
        std::printf("time per frame: %.3f ms median, %.3f ms at the 99th percentile\n", median,
                    slow);
    }
    if (!options.json.empty()) {
        // Totals over the measured frames, as whole numbers, so that two runs compare exactly.
        nlohmann::json run = {
            {"scenario", scenario.name},
            {"gated", scenario.gated},
            {"frames", options.frames},
            {"shown", shown},
            {"columns", columns},
            {"samplesRead", after.samplesRead - before.samplesRead},
            {"samplesReduced", after.samplesReduced - before.samplesReduced},
            {"samplesSummed", after.samplesSummed - before.samplesSummed},
            {"samplesSearched", after.samplesSearched - before.samplesSearched},
            {"matchSearches", after.matchSearches - before.matchSearches},
        };
        if (timed) {
            run["msPerFrameMedian"] = median;
            run["msPerFrameP99"] = slow;
        }
        std::ofstream out(options.json);
        out << run.dump() << '\n';
        if (!out) {
            std::fprintf(stderr, "Could not write %s\n", options.json.c_str());
            return 1;
        }
    }
    if (shown == 0 && !scenario.frozenPan) {
        std::fprintf(stderr, "No frame showed anything: the scenario measures nothing.\n");
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
try {
    const Options options = parse(std::span(argv, static_cast<std::size_t>(argc)));
    if (options.list) {
        for (const bench::Scenario& scenario : bench::kScenarios) {
            std::printf("%s%s\n", std::string(scenario.name).c_str(),
                        scenario.gated ? "" : " (reported only)");
        }
        return 0;
    }
    const auto scenario = bench::scenarioNamed(options.scenario);
    if (!scenario) {
        std::fprintf(stderr, "Unknown scenario \"%s\"; --list names them.\n",
                     options.scenario.c_str());
        return 2;
    }
    return run(*scenario, options);
} catch (const std::exception& error) {
    std::fprintf(stderr, "analysis_bench: %s\n", error.what());
    return 2;
}
