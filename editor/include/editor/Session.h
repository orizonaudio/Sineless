#pragma once

#include "editor/Transport.h"
#include "engine/Analyzer.h"
#include "engine/Engine.h"
#include "engine/Settings.h"

#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>

namespace orizon::editor {

// The engine side of a UI session. C++ is the store of record: the UI changes a
// setting only by asking, and applies only the values this session reports back.
// Used on the UI thread.
class Session
{
public:
    // Display frames do not pass through the session: the page fetches them (see WebView).
    Session(engine::Engine& engine, engine::Analyzer& analyzer, Transport& transport);

    // Handles one message from the page. Before a successful hello only hello is accepted.
    void receive(const nlohmann::json& message);

    // Reports what changed outside the session: settings (the host's setState) as stateChanged,
    // and a new multi-track lane set as lanes; the snapshot again if the audio format changed.
    void sync();

    [[nodiscard]] bool started() const { return isStarted; }

    // macOS 14+: the editor draws the traces (AD-30). Snapshots say so, and whether native drawing
    // has stopped for good; set before the page's hello.
    void setNativeTraces(bool native) { nativeTraces = native; }
    // The diagnostic switch (SINELESS_FRAME_BARCODE=1): snapshots say so, and the page draws the
    // frame barcode on its own traces. Set before the page's hello.
    void setFrameBarcode(bool on) { frameBarcode = on; }
    // Native drawing stopped for good: snapshots say so from now on, and a started page is told.
    void nativeGraphicsLost();

private:
    void hello(const nlohmann::json& message);
    void setSetting(const nlohmann::json& message);
    void setViewport(const nlohmann::json& message);
    void sendSnapshot();
    void sendState(protocol::SettingId id, double value);
    void syncLanes();

    engine::Engine& engine;
    engine::Analyzer& analyzer;
    Transport& transport;
    bool isStarted = false;
    std::optional<engine::Settings> lastSent;
    std::uint32_t lastLaneSet = 0;
    double sentSampleRate = 0.0;
    int sentInputChannels = 0;
    bool nativeTraces = false;
    bool frameBarcode = false;
    bool nativeLost = false;
};

} // namespace orizon::editor
