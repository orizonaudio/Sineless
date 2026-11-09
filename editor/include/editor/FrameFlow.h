#pragma once

#include "editor/UiDispatcher.h"
#include "engine/Analyzer.h"
#include "engine/MonotonicClock.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace orizon::editor {

// Credit-based frame flow: the worker produces a frame only while it holds the single
// credit, which the UI returns after drawing. Production therefore follows the display rate, no
// backlog can build up, and nothing is produced while the page is hidden; the analyzer still
// advances its sweeps then, so they miss nothing.
class FrameFlow
{
public:
    using Deliver = std::function<void(std::span<const std::byte> frame)>;

    // `deliver` runs on the UI thread through `dispatcher`.
    FrameFlow(engine::Analyzer& analyzer, UiDispatcher& dispatcher, Deliver deliver);

    // Any thread: the UI may receive one more frame.
    void grantCredit();

    // Worker thread: produces and hands over a frame if a credit is held.
    void step();

    // macOS native drawing (AD-30). While native, the display link's request is the credit: the
    // worker makes a frame for it, which the display link's thread takes; the page's open request
    // is no credit, and is answered with a copy of the newest frame made for the display that the
    // page has not had, at once or when it next asks. Without a request from the display link the
    // worker advances the sweeps. Any thread.
    void setNative(bool native);
    // Display link thread: the next refresh wants a frame, asked for `lateNanos` after it was due
    // (counted in the time it took).
    void requestForDisplay(std::uint64_t lateNanos = 0);
    // Native: each frame made for the display reports, on the worker, how long it took from when
    // the request was due (the first while it was open) to being ready, by `clock`; none for a
    // request kept open because there was nothing new to show. Set before the worker starts.
    void measureDisplayFrames(std::function<void(std::uint64_t nanos)> made,
                              std::function<std::uint64_t()> clock = engine::monotonicNanos);
    // Display link thread: the newest frame made for the display since the last take, swapped into
    // `into`; false if none.
    bool takeDisplayFrame(std::vector<std::byte>& into);

private:
    void deliverPending();

    engine::Analyzer& analyzer;
    UiDispatcher& dispatcher;
    Deliver deliver;

    std::mutex mutex;
    bool hasCredit = false;
    bool native = false;
    bool displayRequest = false;
    bool displayReady = false;
    std::optional<std::uint64_t> requestedAt; // Unset: none open, or one not to measure.
    std::function<void(std::uint64_t)> madeForDisplay;
    std::function<std::uint64_t()> clock = engine::monotonicNanos;
    bool pageBehind = false;             // Native: `pending` holds a frame the page has not had.
    std::vector<std::byte> displayFrame; // Made for the display, waiting for its thread.
    std::vector<std::byte> pending;      // Produced, waiting for the UI thread.
    std::vector<std::byte> producing;    // Worker-only.
    std::vector<std::byte> delivering;   // UI-thread-only.
};

} // namespace orizon::editor
