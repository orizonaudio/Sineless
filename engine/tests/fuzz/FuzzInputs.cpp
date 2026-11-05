// Writes the fuzzers' dictionary and the session fuzzer's seeds from the protocol's tables and the
// shared message examples, so that a new setting, value or message type reaches the fuzzers
// without anyone updating a list.
// Usage: fuzz_inputs <protocol/tests/data/messages.json> <output directory>
// Writes <output>/fuzz.dict, <output>/seeds/session/* and <output>/seeds/host/*.

#include "HostSteps.h"
#include "engine/SettingJson.h"
#include "protocol/Protocol.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace orizon;

namespace {

// JSON syntax, and numbers at the edges of what a double, an integer or a setting holds.
constexpr auto kJsonTokens = std::to_array<std::string_view>(
    {"{", "}", "[", "]", ":", ",", "\"", "true", "false", "null", "-0", "1e308", "-1e308", "1e-320",
     "4294967296", "18446744073709551616", "-1", "0.5", "\\u0000"});

// Field names of the saved state (engine/src/State.cpp) and of the page's messages
// (protocol/src/messages.ts), which the C++ header does not carry.
constexpr auto kFieldNames = std::to_array<std::string_view>({
    "version",
    "settings",
    "type",
    "name",
    "value",
    "protocolVersion",
    "viewWidth",
    "pixelRatio",
    "frozen",
    "seconds",
    "start",
    "end",
    "ids",
    "on",
});

// One dictionary entry: the token in quotes, with quotes and backslashes escaped.
std::string entry(std::string_view token)
{
    std::string escaped = "\"";
    for (const char c : token) {
        if (c == '"' || c == '\\') {
            escaped += '\\';
        }
        escaped += c;
    }
    return escaped + "\"";
}

bool writeDictionary(const std::filesystem::path& path)
{
    std::set<std::string> names;
    for (const auto& setting : protocol::kSettings) {
        names.emplace(setting.name);
        for (const auto& value : setting.valueNames) {
            names.emplace(value);
        }
    }
    for (const auto& type : protocol::kMessageTypeNames) {
        names.emplace(type);
    }
    for (const auto& field : kFieldNames) {
        names.emplace(field);
    }
    std::ofstream out(path);
    for (const auto& token : kJsonTokens) {
        out << entry(token) << '\n';
    }
    // Names appear as JSON strings: quoted, and quoted with a colon as object keys.
    for (const auto& name : names) {
        out << entry("\"" + name + "\"") << '\n' << entry("\"" + name + "\":") << '\n';
    }
    out << entry(std::to_string(protocol::kProtocolVersion)) << '\n';
    return static_cast<bool>(out);
}

// Every value worth starting from: each enum value, each flag alone, and each number setting's
// minimum, maximum and default.
std::vector<double> seedValues(const protocol::SettingDescriptor& setting)
{
    std::vector<double> values;
    switch (setting.kind) {
    case protocol::SettingKind::kNumber:
        values = {setting.minimum, setting.maximum, setting.defaultValue};
        break;
    case protocol::SettingKind::kEnum:
        for (std::size_t index = 0; index < setting.valueNames.size(); ++index) {
            values.push_back(static_cast<double>(index));
        }
        break;
    case protocol::SettingKind::kFlags:
        for (std::size_t bit = 0; bit < setting.valueNames.size(); ++bit) {
            values.push_back(static_cast<double>(1U << bit));
        }
        break;
    }
    return values;
}

// The session fuzzer reads one message per line, after an optional "<rate> <channels>" line, and
// produces a frame at each empty line. Seeds: a hello, one setting value and a frame, then every
// other example message with a frame after freezing; and all the examples at the edges of the
// sample rates hosts use, in mono and stereo.
bool writeSessionSeeds(const nlohmann::json& examples, const std::filesystem::path& directory)
{
    std::filesystem::remove_all(directory); // No seeds of settings or values since removed.
    std::filesystem::create_directories(directory);
    const std::string hello = examples.at("hello").dump() + "\n";
    std::string others;
    for (const auto& [key, message] : examples.items()) {
        if (message.is_object() && key != "hello") {
            others += message.dump() + (key == "freeze" ? "\n\n" : "\n");
        }
    }
    bool ok = true;
    const auto write = [&](const std::string& name, const std::string& text) {
        std::ofstream out(directory / name);
        out << text;
        ok = ok && static_cast<bool>(out);
    };
    for (const auto& setting : protocol::kSettings) {
        for (const double value : seedValues(setting)) {
            const nlohmann::json json = engine::settingToJson(setting.id, value);
            const nlohmann::json message = {
                {"type", "setSetting"}, {"name", setting.name}, {"value", json}};
            const nlohmann::json& shown = json.is_array() ? json.at(0) : json;
            std::string seed = hello;
            seed += message.dump();
            seed += "\n\n";
            seed += others;
            write(std::string(setting.name) + "-" +
                      (shown.is_string() ? shown.get<std::string>() : shown.dump()),
                  seed);
        }
    }
    // Multi in the Free time base, which draws lanes whatever the host's tempo.
    const auto settingMessage = [](protocol::SettingId id, double value) {
        const auto& setting = protocol::kSettings[static_cast<std::size_t>(id)];
        return nlohmann::json{{"type", "setSetting"},
                              {"name", setting.name},
                              {"value", engine::settingToJson(id, value)}}
                   .dump() +
               "\n";
    };
    write("multi-free", hello +
                            settingMessage(protocol::SettingId::kDisplayMode,
                                           static_cast<double>(protocol::DisplayMode::kMulti)) +
                            settingMessage(protocol::SettingId::kTimeBase,
                                           static_cast<double>(protocol::TimeBase::kFree)) +
                            "\n");
    write("all", hello + "\n" + others);
    write("all-44100-mono", "44100 1\n" + hello + "\n" + others);
    write("all-384000-stereo", "384000 2\n" + hello + "\n" + others);
    return ok;
}

// The host fuzzer's input, step by step, as HostFuzzer.cpp's readSteps reads it.
class HostSeed
{
public:
    using StepKind = fuzz::host::StepKind;
    using Tracks = fuzz::host::Tracks;
    using SignalKind = fuzz::host::SignalKind;

    HostSeed& prepare(Tracks tracks, double rate)
    {
        return rated(StepKind::kPrepare, tracks, rate);
    }
    HostSeed& rate(Tracks tracks, double rate) { return rated(StepKind::kRate, tracks, rate); }
    HostSeed& channels(Tracks tracks, int count)
    {
        return step(StepKind::kChannels).add(tracks).add(count == 1 ? 1 : 0);
    }
    // `blocks` blocks of 4,096 samples of a signal at `hertz`, half scale, none broken or silent.
    HostSeed& play(SignalKind signal, std::uint16_t hertz, int blocks = 8)
    {
        step(StepKind::kPlay).add(signal);
        const unsigned code = hertz - 1U;
        add(static_cast<std::uint8_t>(code & 0xFFU));
        add(static_cast<std::uint8_t>(code >> 8U));
        return add(64).add(1).add(blocks - 1).add(7).add(1).add(1);
    }
    HostSeed& leave() { return step(StepKind::kLeave); }
    HostSeed& join() { return step(StepKind::kJoin); }
    HostSeed& freeze(bool on) { return step(StepKind::kFreeze).add(on ? 1 : 0); }
    // A setting's value: its enum value, or a number's position in its range (0 to 255).
    HostSeed& setting(protocol::SettingId id, std::uint8_t value)
    {
        return step(StepKind::kMessage).add(0).add(id).add(value);
    }
    HostSeed& pan(std::uint8_t at) { return step(StepKind::kMessage).add(1).add(at); }
    HostSeed& advance() { return step(StepKind::kAdvance); }
    HostSeed& frame() { return step(StepKind::kFrame); }

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const { return written; }

private:
    HostSeed& step(StepKind kind) { return add(kind); }
    HostSeed& rated(StepKind kind, Tracks tracks, double rate)
    {
        const auto& rates = fuzz::host::kRates;
        const auto found = std::ranges::find(rates, rate);
        return step(kind).add(tracks).add(found - rates.begin());
    }
    template <typename T> HostSeed& add(T value)
    {
        written.push_back(static_cast<std::uint8_t>(value));
        return *this;
    }

    std::vector<std::uint8_t> written;
};

// The host fuzzer's seeds: each display mode in each time base, played long enough to fill the
// shortest spans, drawn live, frozen, after a rate change while frozen and after a pan; and a track
// leaving and joining again.
bool writeHostSeeds(const std::filesystem::path& directory)
{
    using fuzz::host::SignalKind;
    using fuzz::host::Tracks;
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    bool ok = true;
    const auto write = [&](const std::string& name, const HostSeed& seed) {
        std::ofstream out(directory / name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(seed.bytes().data()),
                  static_cast<std::streamsize>(seed.bytes().size()));
        ok = ok && static_cast<bool>(out);
    };
    const auto& modes =
        protocol::kSettings[static_cast<std::size_t>(protocol::SettingId::kDisplayMode)];
    const auto& bases =
        protocol::kSettings[static_cast<std::size_t>(protocol::SettingId::kTimeBase)];
    for (std::size_t mode = 0; mode < modes.valueNames.size(); ++mode) {
        for (std::size_t base = 0; base < bases.valueNames.size(); ++base) {
            HostSeed seed;
            seed.setting(protocol::SettingId::kDisplayMode, static_cast<std::uint8_t>(mode))
                .setting(protocol::SettingId::kTimeBase, static_cast<std::uint8_t>(base))
                .setting(protocol::SettingId::kFreeSpan, 0)
                .setting(protocol::SettingId::kBeatSpan, 0);
            for (int i = 0; i < 3; ++i) {
                seed.play(SignalKind::kSine, 441).advance();
            }
            seed.frame().freeze(true).frame().rate(Tracks::kBoth, 96000.0).pan(40).frame();
            seed.freeze(false).play(SignalKind::kSquare, 100).frame();
            write(std::string(modes.valueNames[mode]) + "-" + std::string(bases.valueNames[base]),
                  seed);
        }
    }
    // Every rate the input can choose, prepared on both tracks; a mono track, frozen; and a rate
    // change on one track only.
    for (const double rate : fuzz::host::kRates) {
        HostSeed seed;
        seed.prepare(Tracks::kBoth, rate).play(SignalKind::kSine, 441).play(SignalKind::kSine, 441);
        seed.frame();
        write("prepare-" + std::to_string(static_cast<long>(rate)), seed);
    }
    HostSeed mono;
    mono.channels(Tracks::kBoth, 1)
        .play(SignalKind::kSine, 441)
        .frame()
        .freeze(true)
        .frame()
        .channels(Tracks::kBoth, 2)
        .pan(40)
        .frame();
    write("mono", mono);
    HostSeed oneTrack;
    oneTrack.setting(protocol::SettingId::kDisplayMode, 1)
        .play(SignalKind::kSine, 441)
        .rate(Tracks::kOther, 96000.0)
        .play(SignalKind::kSine, 441)
        .frame();
    write("rate-one-track", oneTrack);
    HostSeed tracks;
    tracks.setting(protocol::SettingId::kDisplayMode, 1)
        .play(SignalKind::kSine, 1000)
        .frame()
        .leave()
        .play(SignalKind::kSine, 1000)
        .frame()
        .join()
        .play(SignalKind::kSine, 1000)
        .frame();
    write("tracks", tracks);

    // One seed per oracle, each reaching it (engine/tests/fuzz/CMakeLists.txt requires it): a
    // frozen frame drawn again after a rate change, in Single in both time bases and in Multi; two
    // lanes that mirror each other; and readouts of audio with no finite sample.
    const auto frozenAfterRateChange = [&](const std::string& name, std::uint8_t mode,
                                           std::uint8_t base) {
        HostSeed seed;
        seed.setting(protocol::SettingId::kDisplayMode, mode)
            .setting(protocol::SettingId::kTimeBase, base)
            .setting(protocol::SettingId::kFreeSpan, 0)
            .setting(protocol::SettingId::kBeatSpan, 0);
        for (int i = 0; i < 3; ++i) {
            seed.play(SignalKind::kSine, 441);
        }
        seed.frame().freeze(true).frame().rate(Tracks::kBoth, 96000.0).frame();
        write(name, seed);
    };
    const auto single = static_cast<std::uint8_t>(protocol::DisplayMode::kSingle);
    const auto multi = static_cast<std::uint8_t>(protocol::DisplayMode::kMulti);
    const auto free = static_cast<std::uint8_t>(protocol::TimeBase::kFree);
    const auto beats = static_cast<std::uint8_t>(protocol::TimeBase::kBeats);
    frozenAfterRateChange("frozen-rate-change-single-free", single, free);
    frozenAfterRateChange("frozen-rate-change-single-beats", single, beats);
    frozenAfterRateChange("frozen-rate-change-multi-free", multi, free);
    HostSeed mirrored;
    mirrored.setting(protocol::SettingId::kDisplayMode, multi)
        .setting(protocol::SettingId::kTimeBase, free)
        .setting(protocol::SettingId::kFreeSpan, 0)
        .play(SignalKind::kSine, 441)
        .play(SignalKind::kSine, 441)
        .frame()
        .play(SignalKind::kSquare, 100)
        .frame();
    write("multi-inverted", mirrored);
    HostSeed nonFinite;
    nonFinite.setting(protocol::SettingId::kTimeBase, free)
        .setting(protocol::SettingId::kFreeSpan, 0)
        .play(SignalKind::kNonFinite, 1)
        .play(SignalKind::kNonFinite, 1)
        .frame();
    write("nonfinite-free", nonFinite);
    return ok;
}

} // namespace

int main(int argc, char** argv)
try {
    if (argc != 3) {
        std::cerr << "Usage: fuzz_inputs <messages.json> <output directory>\n";
        return 2;
    }
    std::ifstream examplesFile(argv[1]);
    const nlohmann::json examples = nlohmann::json::parse(examplesFile, nullptr, false);
    if (!examples.is_object() || !examples.contains("hello")) {
        std::cerr << "fuzz_inputs: cannot read the message examples in " << argv[1] << "\n";
        return 1;
    }
    const std::filesystem::path output(argv[2]);
    std::filesystem::create_directories(output);
    if (!writeDictionary(output / "fuzz.dict") ||
        !writeSessionSeeds(examples, output / "seeds" / "session") ||
        !writeHostSeeds(output / "seeds" / "host")) {
        std::cerr << "fuzz_inputs: cannot write to " << output << "\n";
        return 1;
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << "fuzz_inputs: " << error.what() << "\n";
    return 1;
}
