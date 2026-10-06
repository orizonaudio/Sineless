#pragma once

#include "protocol/Protocol.h"

#include <array>
#include <cstdint>
#include <mutex>

namespace orizon::engine {

// One value per setting of DESIGN 1.2, as described by protocol::kSettings. Enum settings hold
// the value index and flags settings a bit mask, both as whole numbers.
class Settings
{
public:
    Settings(); // All defaults.

    [[nodiscard]] double value(protocol::SettingId id) const;

    // Stores `value` if it is valid for the setting and reports whether it did.
    bool set(protocol::SettingId id, double value);

    // Finite and within range; enum and flags values must also be whole numbers.
    [[nodiscard]] static bool isValid(protocol::SettingId id, double value);

    template <typename Enum> [[nodiscard]] Enum enumValue(protocol::SettingId id) const
    {
        return static_cast<Enum>(static_cast<std::uint8_t>(value(id)));
    }

    bool operator==(const Settings& other) const = default;

private:
    std::array<double, protocol::kSettingCount> values{};
};

// The store of record for the settings (DESIGN 5.1). Safe to use from any thread except the
// audio thread, which never reads settings.
class SettingsStore
{
public:
    [[nodiscard]] Settings get() const;
    bool set(protocol::SettingId id, double value);
    void replace(const Settings& settings);

private:
    mutable std::mutex mutex;
    Settings current;
};

} // namespace orizon::engine
