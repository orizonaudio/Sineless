#pragma once

#include <span>

namespace orizon::engine {

// One drawn frame's right edge: when it was drawn (seconds), where its edge stood and the newest
// sample captured then (capture indices).
struct EdgeDraw
{
    double seconds = 0.0;
    double edge = 0.0;
    double newest = 0.0;
};

// How evenly an edge moved over the draws, as a user sees it: per frame, how far it moved against
// how far steady motion at the sample rate would have taken it (a frame that moves too far or too
// little is a stutter; a slow drift is not), and how far it trailed the newest audio. In ms.
struct Motion
{
    double advanceErrorP99 = 0.0;
    double advanceErrorMax = 0.0;
    double lagMax = 0.0;
    bool backwards = false; // The edge ever moved back.
    bool ahead = false;     // The edge ever passed the newest sample.
};

[[nodiscard]] Motion measureMotion(std::span<const EdgeDraw> draws, double sampleRate);

} // namespace orizon::engine
