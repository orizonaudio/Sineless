#include "engine/Settings.h"

#include <cmath>
#include <cstddef>

namespace orizon::engine {

using protocol::SettingId;
using protocol::SettingKind;

namespace {

const protocol::SettingDescriptor& descriptor(SettingId id)
{
    return protocol::kSettings.at(static_cast<std::size_t>(id));
}

} // namespace

Settings::Settings()
{
    for (const auto& setting : protocol::kSettings) {
        values.at(static_cast<std::size_t>(setting.id)) = setting.defaultValue;
    }
}

double Settings::value(SettingId id) const
{
    return values.at(static_cast<std::size_t>(id));
}

bool Settings::set(SettingId id, double value)
{
    if (!isValid(id, value)) {
        return false;
    }
    values.at(static_cast<std::size_t>(id)) = value;
    return true;
}

bool Settings::isValid(SettingId id, double value)
{
    const auto& setting = descriptor(id);
    if (!std::isfinite(value) || value < setting.minimum || value > setting.maximum) {
        return false;
    }
    return setting.kind == SettingKind::kNumber || value == std::floor(value);
}

Settings SettingsStore::get() const
{
    const std::scoped_lock lock(mutex);
    return current;
}

bool SettingsStore::set(SettingId id, double value)
{
    const std::scoped_lock lock(mutex);
    return current.set(id, value);
}

void SettingsStore::replace(const Settings& settings)
{
    const std::scoped_lock lock(mutex);
    current = settings;
}

} // namespace orizon::engine
