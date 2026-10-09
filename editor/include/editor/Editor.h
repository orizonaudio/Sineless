#pragma once

#include "editor/FrameFlow.h"
#include "editor/Session.h"
#include "editor/Transport.h"
#include "editor/UiDispatcher.h"
#include "editor/WebView.h"
#include "engine/AnalysisWorker.h"
#include "engine/Analyzer.h"
#include "engine/Engine.h"
#include "public.sdk/source/common/pluginview.h"

#include <functional>
#include <memory>
#include <string_view>

namespace orizon::editor {

// The VST3 editor view: a WebView filling the host's editor window. Sizes below are in CSS
// pixels; the editor's rect is in the host's window units, scaled by displayScale.
class Editor : public Steinberg::CPluginView
{
public:
    static constexpr int kDefaultWidth = 900;
    static constexpr int kDefaultHeight = 500;
    // The narrowest window that fits the whole control strip and the top bar with its usual
    // status chips (measured), with room for font differences. The UI's .app min-width matches.
    static constexpr int kMinimumWidth = 890;
    static constexpr int kMinimumHeight = 250;
    // Stacked lanes of the multi-track view need this much height each, for their label chip
    // and a clear trace, above the top bar and control strip (kChromeHeight, measured): the
    // minimum height grows with the lanes shown, and the editor asks the host for the room.
    static constexpr int kMinimumLaneHeight = 40;
    static constexpr int kChromeHeight = 140;

    // Sees every raw message from the page before the session handles it. For tests.
    using MessageObserver = std::function<void(std::string_view message)>;

    // `engine` is null when the controller could not reach the processor's engine.
    explicit Editor(std::shared_ptr<engine::Engine> engine, MessageObserver observer = {});

    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API removed() override;
    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect* newSize) override;
    Steinberg::tresult PLUGIN_API canResize() override;
    Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect* size) override;

    // The minimum height now, in CSS pixels: kMinimumHeight, or more with stacked lanes on show.
    [[nodiscard]] int minimumHeight() const;

    // Host window units per CSS pixel.
    [[nodiscard]] double scaleFactor() const { return scale; }

    // False when the editor must show the "unavailable" state.
    [[nodiscard]] bool engineAvailable() const { return engineState != nullptr; }

    // True while attached with an engine: the analysis worker and its per-session state exist
    // only while the editor is open.
    [[nodiscard]] bool workerRunning() const { return worker != nullptr; }
    [[nodiscard]] bool analyzerAllocated() const { return analyzer != nullptr; }

    // The live webview while attached, otherwise null. For tests.
    [[nodiscard]] WebView* webView() const { return view.get(); }

private:
    void receive(std::string_view message);
    // Asks the host for more height when the lanes on show no longer fit, once per change.
    void requestMinimumHeight();
    // Follows a change of display scaling (the window moved to another display): the same size
    // in CSS pixels, in the new window units.
    void updateScale();
    // The current rect converted to `newScale`, which becomes the scale.
    Steinberg::ViewRect rescaled(double newScale);
    [[nodiscard]] int toHostUnits(int cssPixels) const;

    MessageObserver observer;
    double scale = 1.0;
    int lastMinimumHeight = kMinimumHeight;
    std::shared_ptr<engine::Engine> engineState;
    std::unique_ptr<WebView> view;
    std::unique_ptr<WebViewTransport> transport;
    std::unique_ptr<UiDispatcher> dispatcher;
    std::unique_ptr<FrameFlow> frameFlow;
    std::unique_ptr<Session> session;
    std::unique_ptr<engine::Analyzer> analyzer;
    std::unique_ptr<engine::AnalysisWorker> worker; // Declared last: stopped before the rest.
};

} // namespace orizon::editor
