#pragma once

#include "engine/Settings.h"
#include "protocol/Protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace orizon::engine {

// What a test shows: a display mode and the Compare switch, every combination the settings hold.
// Compare is on show only in Multi; Single with the switch on (kept from Multi) shows Single.
enum class Display : std::uint8_t
{
    kSingle,
    kSingleCompareOn,
    kMulti,
    kCompare,
};

static_assert(protocol::kDisplayModeCount * protocol::kToggleCount == 4,
              "a new display mode or switch value needs its Display");

inline protocol::DisplayMode modeOf(Display display)
{
    return display == Display::kSingle || display == Display::kSingleCompareOn
               ? protocol::DisplayMode::kSingle
               : protocol::DisplayMode::kMulti;
}

inline protocol::Toggle compareOf(Display display)
{
    return display == Display::kSingleCompareOn || display == Display::kCompare
               ? protocol::Toggle::kOn
               : protocol::Toggle::kOff;
}

// What `settings` show.
inline Display displayOf(const Settings& settings)
{
    const auto mode = static_cast<std::size_t>(settings.value(protocol::SettingId::kDisplayMode));
    const auto compare = static_cast<std::size_t>(settings.value(protocol::SettingId::kCompare));
    return static_cast<Display>((mode * protocol::kToggleCount) + compare);
}

// Lanes of the group's tracks: Multi, with or without Compare.
inline bool isMulti(Display display)
{
    return modeOf(display) == protocol::DisplayMode::kMulti;
}

// Sets the display mode and the Compare switch, in a Settings or a SettingsStore.
template <typename Store> void show(Store& settings, Display display)
{
    settings.set(protocol::SettingId::kDisplayMode, static_cast<double>(modeOf(display)));
    settings.set(protocol::SettingId::kCompare, static_cast<double>(compareOf(display)));
}

// Every display mode with the switch off and on, from the protocol's enums.
inline std::vector<Display> everyDisplay()
{
    std::vector<Display> displays;
    displays.reserve(protocol::kDisplayModeCount * protocol::kToggleCount);
    for (std::size_t mode = 0; mode < protocol::kDisplayModeCount; ++mode) {
        for (std::size_t compare = 0; compare < protocol::kToggleCount; ++compare) {
            displays.push_back(static_cast<Display>((mode * protocol::kToggleCount) + compare));
        }
    }
    return displays;
}

// For test names: "Single", "SingleCompareOn", "Multi" and "Compare".
inline std::string displayName(Display display)
{
    constexpr std::array kNames{"Single", "SingleCompareOn", "Multi", "Compare"};
    return kNames.at(static_cast<std::size_t>(display));
}

} // namespace orizon::engine
