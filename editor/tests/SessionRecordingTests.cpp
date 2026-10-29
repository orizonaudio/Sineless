// Recorded engine sessions: what the real engine and session send the page, for the app tests to
// replay. Each scenario plays deterministic signals through real instances of a group, connects a
// real Session as the page would, sends the page's messages and the host's events in order, and
// records the snapshot, every message the session sends and the frame each phase ends on, in
// protocol/tests/data/sessions. The app tests replay those files to the real page
// (ui/tests/app/replay-transport.ts), so the page is tested on what only the engine decides, not
// on the mock's idea of it.
//
// Each run records every scenario again and compares it with its file: numbers within the
// corpus's tolerance, frames field by field and point by point. A change in what the engine or
// the session sends in a recorded state fails here on every platform, so a stale recording
// cannot reach the app tests. Set ORIZON_UPDATE_RECORDINGS=1 to write the files from what the
// engine sends now, then review the diff; see "Test levels" in CONTRIBUTING.md.

#include "FrameDecoding.h"
#include "GoldenFiles.h"
#include "Signals.h"
#include "TestHost.h"
#include "editor/Session.h"
#include "editor/Transport.h"
#include "engine/Analyzer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <initializer_list>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orizon::editor {
namespace {

using engine::Analyzer;
using engine::DecodedFrame;
using engine::TestHost;
using engine::Track;
using nlohmann::json;
using protocol::FrameHeader;
using protocol::SettingId;

class RecordingTransport final : public Transport
{
public:
    void sendMessage(const json& message) override { messages.push_back(message); }
    void sendFrame(std::span<const std::byte> /*frame*/) override {}

    // What was sent since the last call, as a JSON array.
    json take()
    {
        json taken = json::array();
        for (auto& message : messages) {
            taken.push_back(std::move(message));
        }
        messages.clear();
        return taken;
    }

    std::vector<json> messages;
};

// ----- Base64, as the page's atob reads it -----

constexpr std::string_view kBase64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string toBase64(std::span<const std::byte> bytes)
{
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::size_t left = bytes.size() - i;
        std::uint32_t triple = std::to_integer<std::uint32_t>(bytes[i]) << 16U;
        if (left > 1) {
            triple |= std::to_integer<std::uint32_t>(bytes[i + 1]) << 8U;
        }
        if (left > 2) {
            triple |= std::to_integer<std::uint32_t>(bytes[i + 2]);
        }
        out += kBase64[(triple >> 18U) & 63U];
        out += kBase64[(triple >> 12U) & 63U];
        out += left > 1 ? kBase64[(triple >> 6U) & 63U] : '=';
        out += left > 2 ? kBase64[triple & 63U] : '=';
    }
    return out;
}

// The bytes of `text`, or none when it is not base64.
std::optional<std::vector<std::byte>> fromBase64(std::string_view text)
{
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::vector<std::byte> out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        std::uint32_t triple = 0;
        int padding = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            const char c = text[i + j];
            std::uint32_t value = 0;
            if (c == '=' && j >= 2) {
                ++padding;
            } else {
                const auto at = kBase64.find(c);
                if (at == std::string_view::npos || padding > 0) {
                    return std::nullopt;
                }
                value = static_cast<std::uint32_t>(at);
            }
            triple = (triple << 6U) | value;
        }
        out.push_back(static_cast<std::byte>((triple >> 16U) & 255U));
        if (padding < 2) {
            out.push_back(static_cast<std::byte>((triple >> 8U) & 255U));
        }
        if (padding < 1) {
            out.push_back(static_cast<std::byte>(triple & 255U));
        }
    }
    return out;
}

// A value of type T at `offset` in a frame.
template <typename T> T read(std::span<const std::byte> bytes, std::size_t offset)
{
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

// ----- Scenarios -----

// One step of a scenario: play audio, a message from the page, or the host changing its rate.
struct Step
{
    enum class Kind : std::uint8_t
    {
        kPlay,
        kPage,
        kHostRate,
    };
    Kind kind = Kind::kPlay;
    double seconds = 0.0;
    // The page's message; or, when `fromFrame` is set, made from the last frame the page got (a
    // value only the engine decides, such as the pan reach).
    json message{};
    std::function<json(std::span<const std::byte>)> fromFrame{};
    std::uint32_t rate = 0;
};

Step play(double seconds)
{
    return {.kind = Step::Kind::kPlay, .seconds = seconds};
}

Step page(json message)
{
    return {.kind = Step::Kind::kPage, .message = std::move(message)};
}

Step pageFrom(std::function<json(std::span<const std::byte>)> make)
{
    return {.kind = Step::Kind::kPage, .fromFrame = std::move(make)};
}

Step hostRate(std::uint32_t rate)
{
    return {.kind = Step::Kind::kHostRate, .rate = rate};
}

struct Scenario
{
    std::string_view name{}; // Test name and file name.
    std::uint32_t rate = 48000;
    // The rate the instances were prepared at, when the host has changed it since without
    // preparing them again (the capture keeps the size it was prepared at).
    std::optional<std::uint32_t> preparedRate{};
    std::vector<Track> tracks{};
    std::vector<std::string> names{}; // Track names the host reports, as Multi and Compare show.
    bool tempoKnown = true;
    std::vector<std::pair<SettingId, double>> settings{}; // Before the page connects.
    std::vector<Step> steps{};
};

// The page's width in CSS pixels and its pixel ratio in the app tests, as both WebKit and
// Chromium report them (viewport 1200 px, vitest.config.ts): frames have the columns the page asks
// for there.
constexpr int kPageWidth = 1182;
constexpr double kPagePixelRatio = 1.0;

constexpr double kSeconds = static_cast<double>(protocol::TimeBase::kSeconds);
constexpr double kBeats = static_cast<double>(protocol::TimeBase::kBeats);
constexpr double kSingle = static_cast<double>(protocol::DisplayMode::kSingle);
constexpr double kMulti = static_cast<double>(protocol::DisplayMode::kMulti);
constexpr double kCompare = static_cast<double>(protocol::DisplayMode::kCompare);
constexpr double kOn = 1.0;

json freeze()
{
    return {{"type", "freeze"}, {"frozen", true}};
}

// The narrowest view, at the centre of the span, as the page asks for it after zooming in at the
// screen's centre until the view stops narrowing (protocol/src/limits.ts, minimumViewWidth): the
// same in WebKit and Chromium.
json deepestZoom()
{
    return {{"type", "view"}, {"start", 0.5 - 0.5 / 256}, {"end", 0.5 + 0.5 / 256}};
}

json panBack(double seconds)
{
    return {{"type", "pan"}, {"seconds", seconds}};
}

json setSetting(std::string_view name, json value)
{
    return {{"type", "setSetting"}, {"name", name}, {"value", std::move(value)}};
}

// clang-format off
std::vector<Scenario> scenarios()
{
    using engine::delayed;
    using engine::mono;
    using engine::noise;
    using engine::silence;
    using engine::sine;
    using engine::stereo;
    const engine::Signal hiss = noise(1, 0.5);
    return {
        // A 1 kHz sine at 44.1 kHz over 10 ms: 441 samples, fewer than the page's columns, so the
        // frame carries raw samples and the hover reads its cycle exactly. Hover on.
        {.name = "SineHalf1k_44k1", .rate = 44100, .tracks = {stereo(sine(1000, 0.5))},
         .settings = {{SettingId::kDisplayMode, kSingle}, {SettingId::kTimeBase, kSeconds},
                      {SettingId::kTimeSpan, 0.01}, {SettingId::kHoverReadout, kOn}},
         .steps = {play(1.0)}},
        // A mono track: the views asked for are L and R (the default), the engine draws L alone.
        // 400 Hz fits four whole cycles into the 10 ms span, so the RMS is exactly a sine's.
        {.name = "MonoSine400_48k", .tracks = {mono(sine(400, 1.0))},
         .settings = {{SettingId::kDisplayMode, kSingle}, {SettingId::kTimeBase, kSeconds},
                      {SettingId::kTimeSpan, 0.01}},
         .steps = {play(1.0)}},
        // Prepared at 192 kHz, then run at 384 kHz without being prepared again: the capture keeps
        // its 192 kHz size. A 2 s span fits what it keeps; then the page asks for 4 s, which does
        // not.
        {.name = "LimitedSpan_384kAfter192k", .rate = 384000, .preparedRate = 192000,
         .tracks = {stereo(sine(100, 0.5))},
         .settings = {{SettingId::kDisplayMode, kSingle}, {SettingId::kTimeBase, kSeconds},
                      {SettingId::kTimeSpan, 2.0}},
         .steps = {play(3.0), page(setSetting("timeSpan", 4.0)), play(0.5)}},
        // Compare at 384 kHz, a copy 499 samples late; the page freezes, then the host changes
        // to 44.1 kHz: the frozen frame still shows audio held at 384 kHz.
        {.name = "FrozenHeldRate_384kThen44k1", .rate = 384000,
         .tracks = {stereo(hiss), stereo(delayed(hiss, 499))}, .names = {"Noise", "Late copy"},
         .settings = {{SettingId::kDisplayMode, kCompare}, {SettingId::kTimeBase, kSeconds},
                      {SettingId::kTimeSpan, 0.01}},
         .steps = {play(2.0), page(freeze()), play(0.1), hostRate(44100), play(0.1)}},
        // Beats, one bar at 120 BPM (2 s), at 384 kHz; frozen after two bars, then 44.1 kHz.
        {.name = "FrozenHeldRateBeats_384kThen44k1", .rate = 384000,
         .tracks = {stereo(sine(100, 0.5))},
         .settings = {{SettingId::kDisplayMode, kSingle}, {SettingId::kTimeBase, kBeats},
                      {SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kOneBar)}},
         .steps = {play(4.0), page(freeze()), play(0.1), hostRate(44100), play(0.1)}},
        // Three named tracks in the multi-track view.
        {.name = "MultiThreeTracks_48k",
         .tracks = {stereo(sine(100, 0.5)), stereo(sine(200, 0.5)), stereo(sine(300, 0.5))},
         .names = {"Kick", "Bass", "Keys"},
         .settings = {{SettingId::kDisplayMode, kMulti}, {SettingId::kTimeBase, kSeconds},
                      {SettingId::kTimeSpan, 0.1}},
         .steps = {play(1.0)}},
        // Two unrelated noises (the corpus's seeds): no clear match.
        {.name = "CompareUnrelated_48k", .tracks = {stereo(hiss), stereo(noise(2, 0.5))},
         .names = {"Noise", "Other noise"},
         .settings = {{SettingId::kDisplayMode, kCompare}, {SettingId::kTimeBase, kSeconds},
                      {SettingId::kTimeSpan, 0.1}},
         .steps = {play(1.0)}},
        // The other track silent: the two never sound together, so nothing is measured (NaN).
        {.name = "CompareOtherSilent_48k", .tracks = {stereo(hiss), stereo(silence())},
         .names = {"Noise", "Silent"},
         .settings = {{SettingId::kDisplayMode, kCompare}, {SettingId::kTimeBase, kSeconds},
                      {SettingId::kTimeSpan, 0.1}},
         .steps = {play(1.0)}},
        // A live 1.5 s roll, zoomed to the narrowest view: the engine draws at least a sample per
        // column, so the frame's visible samples are not the samples in view.
        {.name = "Roll1500msDeepestZoom_48k", .tracks = {stereo(sine(100, 0.5))},
         .settings = {{SettingId::kDisplayMode, kSingle}, {SettingId::kTimeBase, kSeconds},
                      {SettingId::kTimeSpan, 1.5}},
         .steps = {play(2.0), page(deepestZoom()), play(0.2)}},
        // A live Beats sweep of one bar, then zoomed to the narrowest view.
        {.name = "BeatsLive_48k", .tracks = {stereo(sine(100, 0.5))},
         .settings = {{SettingId::kDisplayMode, kSingle}, {SettingId::kTimeBase, kBeats},
                      {SettingId::kBeatSpan, static_cast<double>(protocol::BeatSpan::kOneBar)}},
         .steps = {play(3.0), page(deepestZoom()), play(0.2)}},
        // A 1 s span frozen after 6 s: the page pans back a quarter of the screen, then to as far
        // as the frame says the freeze reaches (the page sends the pan reach it read, a float32).
        {.name = "FrozenPan_48k", .tracks = {stereo(sine(100, 0.5))},
         .settings = {{SettingId::kDisplayMode, kSingle}, {SettingId::kTimeBase, kSeconds},
                      {SettingId::kTimeSpan, 1.0}},
         .steps = {play(6.0), page(freeze()), page(panBack(0.25)),
                   pageFrom([](std::span<const std::byte> frame) {
                       return panBack(read<float>(frame, FrameHeader::kPanReachOffset));
                   })}},
        // A host that reports no tempo, in Beats: the frames carry the group's lane set but no lanes.
        {.name = "NoTempo_Multi_48k", .tracks = {stereo(sine(100, 0.5)), stereo(sine(200, 0.5))},
         .names = {"Kick", "Bass"}, .tempoKnown = false,
         .settings = {{SettingId::kDisplayMode, kMulti}, {SettingId::kTimeBase, kBeats}},
         .steps = {play(1.0)}},
        {.name = "NoTempo_Compare_48k", .tracks = {stereo(sine(100, 0.5)), stereo(sine(200, 0.5))},
         .names = {"Kick", "Bass"}, .tempoKnown = false,
         .settings = {{SettingId::kDisplayMode, kCompare}, {SettingId::kTimeBase, kBeats}},
         .steps = {play(1.0)}},
    };
}
// clang-format on

std::string testName(const ::testing::TestParamInfo<Scenario>& info)
{
    return std::string(info.param.name);
}

// NOLINTNEXTLINE(readability-identifier-naming): the name GoogleTest looks for.
void PrintTo(const Scenario& s, std::ostream* out)
{
    *out << s.name;
}

// ----- Recording -----

struct Recorded
{
    json recording;
    std::vector<std::string> problems; // A scenario that cannot be recorded as it stands.
};

// The most frames produced after a change: Compare's searches and gains spread over frames, so
// the first one after a change may not carry them; a second of frames settles any.
constexpr int kMostFramesAfterAChange = 60;

Recorded record(const Scenario& s)
{
    Recorded out;
    TestHost host(s.preparedRate.value_or(s.rate), s.tracks, s.tempoKnown);
    if (s.preparedRate) {
        host.setSampleRate(s.rate);
    }
    for (std::size_t i = 0; i < s.names.size(); ++i) {
        engine::TrackInfo info;
        info.name = s.names[i];
        host.track(i).setTrackInfo(info);
    }
    for (const auto& [id, value] : s.settings) {
        host.own().settings().set(id, value);
    }
    Analyzer analyzer(host.own(), host.registry);
    RecordingTransport transport;
    Session session(host.own(), analyzer, transport);

    json phases = json::array();
    json phase = {{"after", nullptr},
                  {"answers", json::array()},
                  {"replies", json::array()},
                  {"frames", json::array()}};
    std::vector<std::byte> lastFrame;
    const auto keep = [&](std::vector<std::byte> bytes) {
        phase["frames"] = json::array({toBase64(bytes)});
        lastFrame = std::move(bytes);
    };
    // Produces until nothing changes, as the page keeps asking; keeps the last frame.
    const auto settle = [&] {
        bool produced = false;
        for (int i = 0; i < kMostFramesAfterAChange; ++i) {
            std::vector<std::byte> bytes;
            if (!analyzer.produce(bytes)) {
                break;
            }
            keep(std::move(bytes));
            produced = true;
        }
        return produced;
    };

    session.receive({{"type", "hello"},
                     {"protocolVersion", protocol::kProtocolVersion},
                     {"viewWidth", kPageWidth},
                     {"pixelRatio", kPagePixelRatio}});
    json snapshot;
    for (json& message : transport.take()) {
        if (message.at("type") == "snapshot" && snapshot.is_null()) {
            snapshot = std::move(message);
        } else {
            phase["answers"].push_back(std::move(message));
        }
    }
    settle();

    for (std::size_t index = 0; index < s.steps.size(); ++index) {
        const Step& step = s.steps[index];
        switch (step.kind) {
        case Step::Kind::kPlay: {
            std::optional<DecodedFrame> decoded;
            std::vector<std::byte> bytes;
            host.play(static_cast<std::uint64_t>(std::llround(step.seconds * s.rate)), analyzer,
                      decoded, bytes);
            if (!bytes.empty()) {
                keep(std::move(bytes));
            }
            session.sync(); // As the editor's timer does after audio arrives.
            for (json& message : transport.take()) {
                phase["replies"].push_back(std::move(message));
            }
            break;
        }
        case Step::Kind::kPage: {
            phases.push_back(std::move(phase));
            const json message = step.fromFrame ? step.fromFrame(lastFrame) : step.message;
            session.receive(message);
            phase = {{"after", message},
                     {"answers", transport.take()},
                     {"replies", json::array()},
                     {"frames", json::array()}};
            if (!settle()) {
                out.problems.push_back("step " + std::to_string(index) + " (" + message.dump() +
                                       ") produced no frame: the page would see nothing change");
            }
            break;
        }
        case Step::Kind::kHostRate: {
            phases.push_back(std::move(phase));
            host.setSampleRate(step.rate);
            session.sync();
            // While frozen the engine draws no frame for a rate change: the page keeps the frame
            // it has, so the phase may hold none.
            phase = {{"after", {{"host", "sampleRate"}, {"rate", step.rate}}},
                     {"answers", json::array()},
                     {"replies", transport.take()},
                     {"frames", json::array()}};
            break;
        }
        }
    }
    phases.push_back(std::move(phase));
    out.recording = {{"scenario", s.name},
                     {"protocolVersion", protocol::kProtocolVersion},
                     {"snapshot", std::move(snapshot)},
                     {"phases", std::move(phases)}};
    return out;
}

// ----- Comparison -----

// `parts` joined, without the temporaries that chained operator+ makes.
std::string joined(std::initializer_list<std::string_view> parts)
{
    std::string out;
    for (const std::string_view part : parts) {
        out += part;
    }
    return out;
}

bool near(double approved, double actual)
{
    if (std::isnan(approved) || std::isnan(actual)) {
        return std::isnan(approved) && std::isnan(actual);
    }
    if (std::isinf(approved) || std::isinf(actual)) {
        return approved == actual;
    }
    const double allowed =
        std::max(engine::kAbsoluteTolerance,
                 engine::kRelativeTolerance * std::max(std::abs(approved), std::abs(actual)));
    return std::abs(approved - actual) <= allowed;
}

// The header's integer fields, compared exactly; every other header word is a float32.
constexpr std::array kIntegerWords{FrameHeader::kPointCountOffset, FrameHeader::kLaneSetOffset,
                                   FrameHeader::kInvalidSamplesOffset,
                                   FrameHeader::kInvalidLanesOffset};
constexpr std::size_t kByteFields = 4; // mode, flags, views, laneCount.
static_assert(FrameHeader::kSize == 88,
              "the frame header changed: say here which of its fields are integers, which the "
              "comparison must not take for float32");

void compareFrame(std::span<const std::byte> approved, std::span<const std::byte> actual,
                  const std::string& path, std::vector<std::string>& differences)
{
    if (approved.size() != actual.size()) {
        differences.push_back(path + ": approved " + std::to_string(approved.size()) +
                              " bytes, now " + std::to_string(actual.size()));
        return;
    }
    if (approved.size() < FrameHeader::kSize) {
        differences.push_back(path + ": shorter than a frame header");
        return;
    }
    for (std::size_t offset = 0; offset < kByteFields; ++offset) {
        if (approved[offset] != actual[offset]) {
            differences.push_back(path + ": header byte " + std::to_string(offset) + ": approved " +
                                  std::to_string(std::to_integer<int>(approved[offset])) +
                                  ", now " + std::to_string(std::to_integer<int>(actual[offset])));
        }
    }
    for (std::size_t offset = kByteFields; offset < approved.size(); offset += 4) {
        const bool integer = offset < FrameHeader::kSize &&
                             std::ranges::find(kIntegerWords, offset) != kIntegerWords.end();
        const std::string at = offset < FrameHeader::kSize
                                   ? "header offset " + std::to_string(offset)
                                   : "point " + std::to_string((offset - FrameHeader::kSize) / 4);
        if (integer) {
            const auto a = read<std::uint32_t>(approved, offset);
            const auto b = read<std::uint32_t>(actual, offset);
            if (a != b) {
                differences.push_back(joined({path, ": ", at, ": approved ", std::to_string(a),
                                              ", now ", std::to_string(b)}));
            }
            continue;
        }
        const auto a = read<float>(approved, offset);
        const auto b = read<float>(actual, offset);
        if (!near(a, b)) {
            std::ostringstream text;
            text << path << ": " << at << ": approved " << a << ", now " << b;
            differences.push_back(text.str());
        }
    }
}

void compareJson(const json& approved, const json& actual, const std::string& path,
                 std::vector<std::string>& differences)
{
    if (approved.is_number() && actual.is_number()) {
        // Whole numbers (rates, channel counts, ids) match exactly: the tolerance is for rounding,
        // and would let 384000 pass for 384030.
        const double a = approved.get<double>();
        const double b = actual.get<double>();
        const bool whole =
            std::isfinite(a) && std::isfinite(b) && a == std::trunc(a) && b == std::trunc(b);
        if (whole ? a != b : !near(a, b)) {
            differences.push_back(path + ": approved " + approved.dump() + ", now " +
                                  actual.dump());
        }
        return;
    }
    if (approved.is_array() && actual.is_array()) {
        if (approved.size() != actual.size()) {
            differences.push_back(path + ": approved " + std::to_string(approved.size()) +
                                  " entries, now " + std::to_string(actual.size()));
            return;
        }
        for (std::size_t i = 0; i < approved.size(); ++i) {
            compareJson(approved[i], actual[i], path + "[" + std::to_string(i) + "]", differences);
        }
        return;
    }
    if (approved.is_object() && actual.is_object()) {
        for (const auto& [key, value] : approved.items()) {
            if (!actual.contains(key)) {
                differences.push_back(
                    joined({path, ".", key, ": approved ", value.dump(), ", now gone"}));
            } else {
                compareJson(value, actual.at(key), joined({path, ".", key}), differences);
            }
        }
        for (const auto& [key, value] : actual.items()) {
            if (!approved.contains(key)) {
                differences.push_back(
                    joined({path, ".", key, ": not approved, now ", value.dump()}));
            }
        }
        return;
    }
    if (approved != actual) {
        differences.push_back(path + ": approved " + approved.dump() + ", now " + actual.dump());
    }
}

// Each difference between an approved recording and one made now, by path.
std::vector<std::string> compareRecordings(const json& approved, const json& actual)
{
    std::vector<std::string> differences;
    for (const char* key : {"scenario", "protocolVersion", "snapshot"}) {
        compareJson(approved.value(key, json()), actual.value(key, json()), key, differences);
    }
    const json& a = approved.value("phases", json::array());
    const json& b = actual.value("phases", json::array());
    if (a.size() != b.size()) {
        differences.push_back("phases: approved " + std::to_string(a.size()) + ", now " +
                              std::to_string(b.size()));
        return differences;
    }
    for (std::size_t p = 0; p < a.size(); ++p) {
        const std::string at = "phases[" + std::to_string(p) + "]";
        for (const char* key : {"after", "answers", "replies"}) {
            compareJson(a[p].value(key, json()), b[p].value(key, json()), at + "." + key,
                        differences);
        }
        const json& framesA = a[p].value("frames", json::array());
        const json& framesB = b[p].value("frames", json::array());
        if (framesA.size() != framesB.size()) {
            differences.push_back(at + ".frames: approved " + std::to_string(framesA.size()) +
                                  ", now " + std::to_string(framesB.size()));
            continue;
        }
        for (std::size_t f = 0; f < framesA.size(); ++f) {
            const std::string frameAt = at + ".frames[" + std::to_string(f) + "]";
            const auto bytesA = fromBase64(framesA[f].get<std::string>());
            const auto bytesB = fromBase64(framesB[f].get<std::string>());
            if (!bytesA || !bytesB) {
                differences.push_back(frameAt + ": not base64");
                continue;
            }
            compareFrame(*bytesA, *bytesB, frameAt, differences);
        }
    }
    return differences;
}

std::filesystem::path recordingFile(std::string_view name)
{
    return std::filesystem::path(ORIZON_RECORDINGS_DIR) / (std::string(name) + ".json");
}

// The approved recording in `file`, or none with `problem` saying why.
std::optional<json> loadRecording(const std::filesystem::path& file, std::string& problem)
{
    std::ifstream input(file, std::ios::binary);
    if (!input.is_open()) {
        problem = file.string() +
                  " is missing: run the editor tests with ORIZON_UPDATE_RECORDINGS=1 to write it, "
                  "then review it";
        return std::nullopt;
    }
    std::stringstream text;
    text << input.rdbuf();
    json approved = json::parse(text.str(), nullptr, false);
    if (approved.is_discarded()) {
        problem = file.string() + " is not JSON";
        return std::nullopt;
    }
    return approved;
}

class SessionRecording : public ::testing::TestWithParam<Scenario>
{
};

TEST_P(SessionRecording, MatchesTheRecordingTheAppTestsReplay)
{
    const Scenario& s = GetParam();
    const Recorded recorded = record(s);
    for (const std::string& problem : recorded.problems) {
        ADD_FAILURE() << problem;
    }
    const std::filesystem::path file = recordingFile(s.name);
    if (engine::updating("ORIZON_UPDATE_RECORDINGS")) {
        std::filesystem::create_directories(file.parent_path());
        // Not engine::serialise: its rewrite of exponents would reach into the frames' base64.
        // Prettier leaves these files alone (.prettierignore), and std::ios::binary keeps LF.
        std::ofstream(file, std::ios::binary) << recorded.recording.dump(2) << "\n";
        GTEST_SKIP() << "wrote " << file.string();
    }
    std::string problem;
    const std::optional<json> approved = loadRecording(file, problem);
    ASSERT_TRUE(approved) << problem;
    const std::vector<std::string> differences = compareRecordings(*approved, recorded.recording);
    std::string listed;
    for (const std::string& difference : differences) {
        listed += "\n  " + difference;
    }
    EXPECT_TRUE(differences.empty())
        << s.name << " sends the page something else than its recording:" << listed
        << "\nIf the change is intended, write the recordings again with "
           "ORIZON_UPDATE_RECORDINGS=1 and review the diff.";
}

INSTANTIATE_TEST_SUITE_P(Scenarios, SessionRecording, ::testing::ValuesIn(scenarios()), testName);

// ----- The comparison and the recorder themselves -----

// A recording of a scenario that holds NaN values (Compare with the other track silent), made
// once for the tests below.
const json& sample()
{
    static const json kRecording = [] {
        for (const Scenario& s : scenarios()) {
            if (s.name == "CompareOtherSilent_48k") {
                return record(s).recording;
            }
        }
        return json();
    }();
    return kRecording;
}

// The first frame of the last phase that holds one, as bytes.
std::vector<std::byte> lastFrameOf(const json& recording)
{
    const json& phases = recording.at("phases");
    for (auto it = phases.rbegin(); it != phases.rend(); ++it) {
        if (!it->at("frames").empty()) {
            return fromBase64(it->at("frames")[0].get<std::string>())
                .value_or(std::vector<std::byte>{});
        }
    }
    return {};
}

json withLastFrame(json recording, std::span<const std::byte> bytes)
{
    json& phases = recording.at("phases");
    for (auto it = phases.rbegin(); it != phases.rend(); ++it) {
        if (!it->at("frames").empty()) {
            it->at("frames")[0] = toBase64(bytes);
            break;
        }
    }
    return recording;
}

void writeFloat(std::vector<std::byte>& bytes, std::size_t offset, float value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

TEST(SessionRecordingComparison, PassesARecordingWithNaNValuesComparedWithItself)
{
    ASSERT_FALSE(sample().is_null());
    const std::vector<std::byte> frame = lastFrameOf(sample());
    ASSERT_GE(frame.size(), FrameHeader::kSize);
    EXPECT_TRUE(std::isnan(read<float>(frame, FrameHeader::kFrequencyOffset)))
        << "the sample should carry a NaN";
    EXPECT_TRUE(compareRecordings(sample(), sample()).empty());
}

TEST(SessionRecordingComparison, PassesAPointMovedWithinTheToleranceAndFailsOneMovedPastIt)
{
    std::vector<std::byte> frame = lastFrameOf(sample());
    ASSERT_GT(frame.size(), FrameHeader::kSize);
    const std::size_t offset = FrameHeader::kSize;
    const auto value = read<float>(frame, offset);
    const double allowed =
        std::max(engine::kAbsoluteTolerance,
                 engine::kRelativeTolerance * std::abs(static_cast<double>(value)));
    writeFloat(frame, offset, static_cast<float>(value + allowed / 2));
    EXPECT_TRUE(compareRecordings(sample(), withLastFrame(sample(), frame)).empty());
    writeFloat(frame, offset, static_cast<float>(value + allowed * 2));
    const auto differences = compareRecordings(sample(), withLastFrame(sample(), frame));
    ASSERT_EQ(differences.size(), 1u);
    EXPECT_NE(differences[0].find("point 0"), std::string::npos) << differences[0];
}

TEST(SessionRecordingComparison, ComparesAHeaderIntegerExactly)
{
    std::vector<std::byte> frame = lastFrameOf(sample());
    const auto points = read<std::uint32_t>(frame, FrameHeader::kPointCountOffset);
    const std::uint32_t changed = points + 1;
    std::memcpy(frame.data() + FrameHeader::kPointCountOffset, &changed, sizeof(changed));
    const auto differences = compareRecordings(sample(), withLastFrame(sample(), frame));
    ASSERT_EQ(differences.size(), 1u);
    EXPECT_NE(differences[0].find("header offset 4"), std::string::npos) << differences[0];
}

TEST(SessionRecordingComparison, PassesAMessageNumberWithinTheToleranceAndFailsOnePastIt)
{
    const json approved = {{"phases", json::array({{{"after", {{"type", "view"}, {"start", 0.5}}},
                                                    {"answers", json::array()},
                                                    {"replies", json::array()},
                                                    {"frames", json::array()}}})}};
    json within = approved;
    within["phases"][0]["after"]["start"] = 0.5 * (1 + engine::kRelativeTolerance / 2);
    EXPECT_TRUE(compareRecordings(approved, within).empty());
    json past = approved;
    past["phases"][0]["after"]["start"] = 0.5 * (1 + engine::kRelativeTolerance * 2);
    const auto differences = compareRecordings(approved, past);
    ASSERT_EQ(differences.size(), 1u);
    EXPECT_NE(differences[0].find("phases[0].after.start"), std::string::npos) << differences[0];
}

TEST(SessionRecordingComparison, ComparesWholeNumbersInMessagesExactly)
{
    const json approved = {{"snapshot", {{"sampleRate", 384000}}}};
    json changed = approved;
    changed["snapshot"]["sampleRate"] = 384030; // Within the relative tolerance.
    const auto differences = compareRecordings(approved, changed);
    ASSERT_EQ(differences.size(), 1u);
    EXPECT_NE(differences[0].find("snapshot.sampleRate"), std::string::npos) << differences[0];
}

TEST(SessionRecordingComparison, FailsAChangedMessageField)
{
    json changed = sample();
    changed["snapshot"]["inputChannels"] = 1;
    const auto differences = compareRecordings(sample(), changed);
    ASSERT_EQ(differences.size(), 1u);
    EXPECT_NE(differences[0].find("snapshot.inputChannels"), std::string::npos) << differences[0];
}

TEST(SessionRecordingComparison, FailsAMissingFrameAMissingPhaseAndAnExtraPhase)
{
    json missingFrame = sample();
    for (json& phase : missingFrame["phases"]) {
        if (!phase["frames"].empty()) {
            phase["frames"] = json::array();
            break;
        }
    }
    EXPECT_FALSE(compareRecordings(sample(), missingFrame).empty());
    json missingPhase = sample();
    missingPhase["phases"].erase(missingPhase["phases"].size() - 1);
    EXPECT_FALSE(compareRecordings(sample(), missingPhase).empty());
    json extraPhase = sample();
    extraPhase["phases"].push_back(extraPhase["phases"].back());
    EXPECT_FALSE(compareRecordings(sample(), extraPhase).empty());
}

TEST(SessionRecordingComparison, SaysHowToWriteAMissingRecording)
{
    std::string problem;
    EXPECT_FALSE(loadRecording(recordingFile("NoSuchScenario"), problem));
    EXPECT_NE(problem.find("ORIZON_UPDATE_RECORDINGS=1"), std::string::npos) << problem;
}

TEST(SessionRecorder, FailsAPageMessageAfterWhichNothingChanges)
{
    // Unfreezing a live display changes nothing, so no frame follows.
    Scenario s = scenarios().front();
    s.steps = {play(0.1), page({{"type", "freeze"}, {"frozen", false}})};
    const Recorded recorded = record(s);
    ASSERT_EQ(recorded.problems.size(), 1u);
    EXPECT_NE(recorded.problems[0].find("step 1"), std::string::npos) << recorded.problems[0];
}

TEST(SessionRecorder, AcceptsAHostRateChangeWhileFrozenThatDrawsNoFrame)
{
    const Scenario s = [] {
        for (const Scenario& candidate : scenarios()) {
            if (candidate.name == "FrozenHeldRate_384kThen44k1") {
                return candidate;
            }
        }
        return Scenario{};
    }();
    const Recorded recorded = record(s);
    EXPECT_TRUE(recorded.problems.empty());
    const json& phases = recorded.recording.at("phases");
    const auto host = std::ranges::find_if(
        phases, [](const json& phase) { return phase.at("after").contains("host"); });
    ASSERT_NE(host, phases.end());
    EXPECT_TRUE(host->at("frames").empty()) << "frozen, a rate change draws no frame";
    EXPECT_FALSE(host->at("replies").empty()) << "the page gets the host's new rate";
}

TEST(SessionRecordingBase64, RoundTripsEveryLengthModuloThree)
{
    for (std::size_t length : {0U, 1U, 2U, 3U, 4U, 5U}) {
        std::vector<std::byte> bytes(length);
        for (std::size_t i = 0; i < length; ++i) {
            bytes[i] = static_cast<std::byte>(250 + i);
        }
        EXPECT_EQ(fromBase64(toBase64(bytes)), bytes) << length;
    }
    EXPECT_FALSE(fromBase64("abc"));
    EXPECT_FALSE(fromBase64("a=bc"));
}

} // namespace
} // namespace orizon::editor
