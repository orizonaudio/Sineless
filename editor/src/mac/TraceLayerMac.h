#pragma once

#include "editor/TraceView.h"
#include "engine/FramePacing.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#ifdef __OBJC__
#import <AppKit/AppKit.h>
#endif

namespace orizon::editor {

// The traces drawn natively on macOS (AD-30): a Metal layer below the transparent page, at the
// page's screen rectangle, presented from a CAMetalDisplayLink on a thread of its own. Each
// callback steps the display clock (FramePacing), draws the newest frame made for the display with
// the page's newest trace view when either changed, presents it, and asks for the frame the next
// shown refresh needs. No step passes through the host's main thread.
//
// Hidden, with its link paused, until the page has sent a trace view, while the page is hidden,
// while its window is off screen or occluded and while the screen is shorter than the chrome.
// Native graphics lost (a removed GPU, revoked access, command buffers failing for
// kFailureSecondsBeforeLost, refused graphics memory) stops it for good: it keeps its last picture
// and reports once.
class TraceLayer
{
public:
    // Command buffers failing for this long, whatever the error, stop native drawing: long enough
    // for a GPU reset to recover, and measured in time so it means the same at any refresh rate.
    static constexpr double kFailureSecondsBeforeLost = 0.5;

    struct Hooks
    {
        // Render thread: the next shown refresh wants a frame (FrameFlow::requestForDisplay, and
        // the worker woken).
        std::function<void()> requestFrame;
        // Render thread: the newest frame made for the display since the last take, swapped into
        // `into`; false if none (FrameFlow::takeDisplayFrame).
        std::function<bool(std::vector<std::byte>& into)> takeFrame;
        // Main thread, at most once: native drawing stopped for good.
        std::function<void()> lost;
    };

    virtual ~TraceLayer() = default;
    TraceLayer(const TraceLayer&) = delete;
    TraceLayer& operator=(const TraceLayer&) = delete;

#ifdef __OBJC__
    // In `container`, below `above` (the web view); laid out at once. `pacing` and the hooks'
    // targets must outlive it: destroying it stops every callback before it returns.
    static std::unique_ptr<TraceLayer> create(NSView* container, NSView* above,
                                              engine::FramePacing& pacing, Hooks hooks);
    [[nodiscard]] virtual NSView* view() const = 0;
#endif

    // Main thread. The page's newest trace view (the first since the page was shown shows the
    // layer).
    virtual void setTraceView(const TraceView& view) = 0;
    // Main thread. The container's size or backing scale changed: the screen's rectangle and the
    // drawable's size again.
    virtual void layout() = 0;
    // Main thread. The page was shown or hidden (loaded, or gone after a crash); hidden, the layer
    // waits for the next page's first trace view.
    virtual void setPageShown(bool shown) = 0;
    // Native drawing stopped for good. Any thread.
    [[nodiscard]] virtual bool lost() const = 0;

    // For tests.
    struct Stats
    {
        std::uint64_t callbacks = 0;
        std::uint64_t presented = 0;
        // The size of the drawable last drawn into, in device pixels.
        std::size_t drawnWidth = 0;
        std::size_t drawnHeight = 0;
    };
    [[nodiscard]] virtual Stats statsForTesting() const = 0;
    // Command buffers from now on complete as failed with this MTLCommandBufferError code (none:
    // as Metal reports them).
    virtual void failCommandBuffersForTesting(std::optional<long> code) = 0;
    // The coverage targets' next allocations are refused, as Metal refusing the memory would.
    virtual void refuseCoverageTargetsForTesting() = 0;
    // Layers take their windows as occluded or not, as the window server's notifications then say
    // (none: as it reports them). A test machine's idle display reports every window occluded.
    static void setOcclusionForTesting(std::optional<bool> occluded);

protected:
    TraceLayer() = default;
};

} // namespace orizon::editor
