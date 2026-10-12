// The JSON form of setting values against malformed input: saved state and the page's setSetting
// messages both reach settingFromJson. The input is tried as the value of every setting, and an
// object's members also as the value of the setting they name. A value read must convert back to
// the same JSON form and read again as the same value.

#include "FuzzCheck.h"
#include "engine/SettingJson.h"

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

using orizon::fuzz::require;
using namespace orizon;

namespace {

void check(protocol::SettingId id, const nlohmann::json& json)
{
    const auto value = engine::settingFromJson(id, json);
    if (!value) {
        return;
    }
    const nlohmann::json written = engine::settingToJson(id, *value);
    const auto reread = engine::settingFromJson(id, written);
    require(reread.has_value(), "a value read converts to JSON that reads again");
    if (!reread) {
        return;
    }
    require(*reread == *value, "a value read reads again as the same value");
    require(engine::settingToJson(id, *reread) == written, "writing a value is stable");
}

} // namespace

// The entry point libFuzzer calls, by its name.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const nlohmann::json json = nlohmann::json::parse(data, data + size, nullptr, false);
    if (json.is_discarded()) {
        return 0;
    }
    for (const auto& setting : protocol::kSettings) {
        check(setting.id, json);
        if (json.is_object()) {
            if (const auto found = json.find(std::string(setting.name)); found != json.end()) {
                check(setting.id, *found);
            }
        }
    }
    return 0;
}
