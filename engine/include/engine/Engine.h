#pragma once

#include "engine/CaptureRing.h"
#include "engine/Settings.h"
#include "engine/Timeline.h"

#include <atomic>

namespace orizon::engine {

// Everything one plugin instance shares between its audio thread and its editor.
// The processor creates it; the controller reaches it through an in-process handle.
class Engine
{
public:
    Engine() = default;

    [[nodiscard]] CaptureRing& captureRing() { return ring; }
    [[nodiscard]] const CaptureRing& captureRing() const { return ring; }

    [[nodiscard]] Timeline& timeline() { return timelineState; }
    [[nodiscard]] const Timeline& timeline() const { return timelineState; }

    [[nodiscard]] SettingsStore& settings() { return settingsStore; }
    [[nodiscard]] const SettingsStore& settings() const { return settingsStore; }

    // Main input channel count (1 or 2), set by the processor on bus negotiation.
    void setInputChannels(int count) { inputChannelCount.store(count, std::memory_order_relaxed); }
    [[nodiscard]] int inputChannels() const
    {
        return inputChannelCount.load(std::memory_order_relaxed);
    }

    // Set by the processor in setupProcessing; read by the analysis worker.
    void setSampleRate(double rate) { sampleRateValue.store(rate, std::memory_order_relaxed); }
    [[nodiscard]] double sampleRate() const
    {
        return sampleRateValue.load(std::memory_order_relaxed);
    }

private:
    CaptureRing ring;
    std::atomic<double> sampleRateValue{48000.0};
    std::atomic<int> inputChannelCount{2};
    SettingsStore settingsStore;
    Timeline timelineState;
};

} // namespace orizon::engine
