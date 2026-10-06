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

// The VST3 editor view: a WebView filling the host's editor window.
class Editor : public Steinberg::CPluginView
{
public:
    static constexpr int kDefaultWidth = 800;
    static constexpr int kDefaultHeight = 500;
    static constexpr int kMinimumWidth = 400;
    static constexpr int kMinimumHeight = 250;

    // Sees every raw message from the page before the session handles it. For tests.
    using MessageObserver = std::function<void(std::string_view message)>;

    // `engine` is null when the controller could not reach the processor's engine (AD-10).
    explicit Editor(std::shared_ptr<engine::Engine> engine, MessageObserver observer = {});

    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API removed() override;
    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect* newSize) override;
    Steinberg::tresult PLUGIN_API canResize() override;
    Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect* rect) override;

    // False when the editor must show the "unavailable" state (AD-10).
    [[nodiscard]] bool engineAvailable() const { return engineState != nullptr; }

    // True while attached with an engine: the analysis worker and its per-session state exist
    // only while the editor is open.
    [[nodiscard]] bool workerRunning() const { return worker != nullptr; }
    [[nodiscard]] bool analyzerAllocated() const { return analyzer != nullptr; }

    // The live webview while attached, otherwise null. For tests.
    [[nodiscard]] WebView* webView() const { return view.get(); }

private:
    void receive(std::string_view message);

    MessageObserver observer;
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
