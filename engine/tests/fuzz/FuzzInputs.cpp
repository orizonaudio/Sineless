// Writes the fuzzers' dictionary and the session fuzzer's seeds from the protocol's tables and the
// shared message examples, so that a new setting, value or message type reaches the fuzzers
// without anyone updating a list.
// Usage: fuzz_inputs <protocol/tests/data/messages.json> <output directory>
// Writes <output>/fuzz.dict and <output>/seeds/session/*.

#include "engine/SettingJson.h"
#include "protocol/Protocol.h"

#include <array>
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
    write("all", hello + "\n" + others);
    write("all-44100-mono", "44100 1\n" + hello + "\n" + others);
    write("all-384000-stereo", "384000 2\n" + hello + "\n" + others);
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
        !writeSessionSeeds(examples, output / "seeds" / "session")) {
        std::cerr << "fuzz_inputs: cannot write to " << output << "\n";
        return 1;
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << "fuzz_inputs: " << error.what() << "\n";
    return 1;
}
