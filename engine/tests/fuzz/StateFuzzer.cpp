// Saved-state loading against malformed input: the host hands setState whatever bytes it kept,
// from any version of the plugin, or damaged. Loading must never crash, must keep only valid
// values, and what it keeps must survive a save and a reload unchanged.

#include "FuzzCheck.h"
#include "engine/Settings.h"
#include "engine/State.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

using orizon::fuzz::require;
using namespace orizon;

// The entry point libFuzzer calls, by its name.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    const engine::SavedState loaded = engine::deserializeState(text);
    for (const auto& setting : protocol::kSettings) {
        require(engine::Settings::isValid(setting.id, loaded.settings.value(setting.id)),
                "a loaded setting is valid");
    }

    const std::string saved = engine::serializeState(loaded.settings, loaded.editorSize);
    const engine::SavedState reloaded = engine::deserializeState(saved);
    require(reloaded.settings == loaded.settings, "saving and reloading keeps every setting");
    require(reloaded.editorSize == loaded.editorSize, "saving and reloading keeps the editor size");
    require(engine::serializeState(reloaded.settings, reloaded.editorSize) == saved,
            "saving a reloaded state is stable");
    return 0;
}
