#pragma once

#include "editor/EditorCore.h"
#include "editor/WebView.h"
#include "engine/Engine.h"
#include "public.sdk/source/common/pluginview.h"

#include <memory>

namespace orizon::editor {

// The VST3 editor view: the host's editor window around an EditorCore, which holds everything
// that is not VST3. Sizes below are in CSS pixels; the editor's rect is in the host's window
// units, scaled by displayScale.
class Editor : public Steinberg::CPluginView
{
public:
    static constexpr int kMinimumWidth = EditorCore::kMinimumWidth;
    static constexpr int kMinimumHeight = EditorCore::kMinimumHeight;
    static constexpr int kDefaultWidth = EditorCore::kDefaultWidth;
    static constexpr int kDefaultHeight = EditorCore::kDefaultHeight;
    static constexpr int kMinimumLaneHeight = EditorCore::kMinimumLaneHeight;
    static constexpr int kChromeHeight = EditorCore::kChromeHeight;

    using MessageObserver = EditorCore::MessageObserver;

    // `engine` is null when the controller could not reach the processor's engine.
    explicit Editor(std::shared_ptr<engine::Engine> engine, MessageObserver observer = {});

    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API removed() override;
    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect* newSize) override;
    Steinberg::tresult PLUGIN_API canResize() override;
    Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect* size) override;

    [[nodiscard]] int minimumHeight() const { return core.minimumHeight(); }
    [[nodiscard]] static int minimumHeightFor(const engine::Settings& settings, int inputChannels,
                                              const engine::LaneSet& laneSet)
    {
        return EditorCore::minimumHeightFor(settings, inputChannels, laneSet);
    }
    [[nodiscard]] double scaleFactor() const { return core.scaleFactor(); }
    [[nodiscard]] bool engineAvailable() const { return core.engineAvailable(); }
    [[nodiscard]] bool workerRunning() const { return core.workerRunning(); }
    [[nodiscard]] bool analyzerAllocated() const { return core.analyzerAllocated(); }
    [[nodiscard]] WebView* webView() const { return core.webView(); }

private:
    // Asks the host for `wanted` (host units). By the VST3 contract the host answers with onSize
    // in the same call, or declines; some (Live and Bitwig on Windows, REAPER and WaveLab on
    // macOS, as JUCE lists them) resize their window and accept without calling onSize, and the
    // size is then applied here.
    void requestSize(Steinberg::ViewRect wanted);
    // The core's size at the current origin.
    [[nodiscard]] Steinberg::ViewRect hostRect() const;

    EditorCore core;
    bool sizedByHost = false; // Set by onSize, for requestSize.
};

} // namespace orizon::editor
