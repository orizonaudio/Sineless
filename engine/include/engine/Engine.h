#pragma once

#include "engine/CaptureRing.h"
#include "engine/Settings.h"

#include <atomic>

namespace orizon::engine {

// Everything one plugin instance shares between its audio thread and its editor (DESIGN 5.2).
// The processor creates it; the controller reaches it through an in-process handle (AD-10).
class Engine
{
public:
    Engine() = default;

    [[nodiscard]] CaptureRing& captureRing() { return ring; }
    [[nodiscard]] const CaptureRing& captureRing() const { return ring; }

    [[nodiscard]] SettingsStore& settings() { return settingsStore; }
    [[nodiscard]] const SettingsStore& settings() const { return settingsStore; }

    // Set by the processor in setupProcessing; read by the analysis worker.
    void setSampleRate(double rate) { sampleRateValue.store(rate, std::memory_order_relaxed); }
    [[nodiscard]] double sampleRate() const
    {
        return sampleRateValue.load(std::memory_order_relaxed);
    }

private:
    CaptureRing ring;
    std::atomic<double> sampleRateValue{48000.0};
    SettingsStore settingsStore;
};

} // namespace orizon::engine
