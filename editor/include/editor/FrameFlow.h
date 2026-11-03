#pragma once

#include "editor/UiDispatcher.h"
#include "engine/Analyzer.h"

#include <cstddef>
#include <functional>
#include <mutex>
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
    // is no credit, and is answered with a copy of the next frame made for the display. Without a
    // request from the display link the worker advances the sweeps. Any thread.
    void setNative(bool native);
    // Display link thread: the next refresh wants a frame.
    void requestForDisplay();
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
    std::vector<std::byte> displayFrame; // Made for the display, waiting for its thread.
    std::vector<std::byte> pending;      // Produced, waiting for the UI thread.
    std::vector<std::byte> producing;    // Worker-only.
    std::vector<std::byte> delivering;   // UI-thread-only.
};

} // namespace orizon::editor
