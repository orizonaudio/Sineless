#pragma once

#include "engine/CaptureRing.h"

namespace orizon::engine {

// Everything one plugin instance shares between its audio thread and its editor (DESIGN 5.2).
// The processor creates it; the controller reaches it through an in-process handle (AD-10).
class Engine
{
public:
    Engine() = default;

    [[nodiscard]] CaptureRing& captureRing() { return ring; }
    [[nodiscard]] const CaptureRing& captureRing() const { return ring; }

private:
    CaptureRing ring;
};

} // namespace orizon::engine
