#pragma once

#include "engine/Engine.h"
#include "engine/HoldBuffer.h"
#include "engine/Settings.h"
#include "engine/Trigger.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace orizon::engine {

// Per-editor-session analysis (AD-2, DESIGN 5.2): turns the capture into display frames. It is
// created when the editor opens, so the hold buffer is allocated only then.
//
// Commands may come from any non-audio thread; produce() runs on the analysis worker.
class Analyzer
{
public:
    explicit Analyzer(Engine& engine);

    // Device-pixel width of the trace area; no frames are produced while it is 0.
    void setColumns(std::size_t columns);
    void setFrozen(bool frozen);
    // While frozen: how far back to move the view, in seconds (FR-6).
    void setPan(double seconds);
    // Single mode: wait for the next trigger again.
    void arm();

    // Writes the next frame into `frame`. Returns false when there is nothing new to show.
    bool produce(std::vector<std::byte>& frame);

private:
    struct Commands
    {
        std::size_t columns = 0;
        bool frozen = false;
        double panSeconds = 0.0;
        bool armRequested = false;
    };

    // The last window shown while live, which a freeze holds on to.
    struct Window
    {
        double start = 0.0;
        double span = 0.0;
        bool triggered = false;
        bool roll = false;
        // Tempo-synced sweep (FR-9): samples rotated left by `rotation`; cursor in 0 to 1.
        bool synced = false;
        bool limited = false;
        std::uint64_t rotation = 0;
        double cursor = -1.0;
        double cycleStart = 0.0;
        double spanQuarterNotes = 0.0;
        double barQuarterNotes = 0.0;
    };

    Commands takeCommands();
    bool freeze(double sampleRate, std::uint64_t minimumSamples = 0);
    bool render(const SampleSource& source, const Settings& settings, double sampleRate,
                const Window& window, std::uint64_t sampleCount, double samplesPerColumn,
                std::size_t columns, std::vector<std::byte>& frame);

    Engine& engine;
    HoldBuffer hold;
    TriggerState trigger;

    std::mutex commandMutex;
    Commands commands;

    // Worker-only state.
    bool isFrozen = false;
    std::optional<Window> lastLive;
    std::optional<Window> lastShown;
    std::optional<Settings> lastSettings;
    std::size_t lastColumns = 0;
    double lastPan = 0.0;
    bool lastFrozen = false;
    std::uint32_t sequence = 0;
    // Tempo sync: the clock and capture end when frozen, and the capture end of the last frame.
    MusicalStamp frozenStamp;
    std::uint64_t frozenEnd = 0;
    std::uint64_t lastEnd = 0;
    std::vector<float> triggerSamples;
    std::vector<float> viewSamples;
    std::vector<float> scratch;
    std::vector<float> reduced;
    std::vector<float> frameData;
};

} // namespace orizon::engine
