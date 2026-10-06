#pragma once

#include "editor/WebView.h"
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

    explicit Editor(WebView::MessageHandler onMessage);

    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API removed() override;
    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect* newSize) override;
    Steinberg::tresult PLUGIN_API canResize() override;
    Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect* rect) override;

    // The live webview while attached, otherwise null. For tests.
    [[nodiscard]] WebView* webView() const { return view.get(); }

  private:
    WebView::MessageHandler onMessage;
    std::unique_ptr<WebView> view;
};

} // namespace orizon::editor
