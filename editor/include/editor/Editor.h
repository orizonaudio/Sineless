#pragma once

#include "editor/WebView.h"
#include "engine/AnalysisWorker.h"
#include "engine/Engine.h"
#include "public.sdk/source/common/pluginview.h"

#include <memory>

namespace orizon::editor {

// The VST3 editor view: a WebView filling the host's editor window.
class Editor : public Steinberg::CPluginView
{
public:
    static constexpr int kDefaultWidth = 800;
    static constexpr int kDefaultHeight = 500;
    static constexpr int kMinimumWidth = 400;
    static constexpr int kMinimumHeight = 250;

    // `engine` is null when the controller could not reach the processor's engine (AD-10).
    Editor(WebView::MessageHandler onMessage, std::shared_ptr<engine::Engine> engine);

    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API removed() override;
    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect* newSize) override;
    Steinberg::tresult PLUGIN_API canResize() override;
    Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect* rect) override;

    // False when the editor must show the "unavailable" state (AD-10).
    [[nodiscard]] bool engineAvailable() const { return engineState != nullptr; }

    // True while attached with an engine: the analysis worker runs only while the editor is open.
    [[nodiscard]] bool workerRunning() const { return worker != nullptr; }

    // The live webview while attached, otherwise null. For tests.
    [[nodiscard]] WebView* webView() const { return view.get(); }

private:
    WebView::MessageHandler onMessage;
    std::shared_ptr<engine::Engine> engineState;
    std::unique_ptr<WebView> view;
    std::unique_ptr<engine::AnalysisWorker> worker;
};

} // namespace orizon::editor
