#include "editor/PointerHold.h"

#include <cmath>
#include <utility>

namespace orizon::editor {

namespace {
// A pointer closer than this to its press, in each axis, is not warped back. A frozen pointer is
// where the hold found it, the press point but for motion before the start, so this is what a click
// leaves; skipping the warp posts nothing that could delay a double-click's second press. Under a
// point (a CSS px) the difference is not visible, and covers a fresh position read differing from
// the press event's location by a fraction.
constexpr double kWarpDistance = 1;
} // namespace

PointerHold::PointerHold(PointerControl& control, Report report)
    : control(control), report(std::move(report))
{}

PointerHold::~PointerHold()
{
    end();
    *alive = false;
}

void PointerHold::pressed(ScreenPoint point)
{
    press = point;
}

void PointerHold::start()
{
    if (held || !press || !control.primaryButtonDown()) {
        return;
    }
    heldPress = *press;
    press.reset();
    held = true;
    // The page's own travel so far, read with the time that later events are measured against.
    travel = control.position().y - heldPress.y;
    startTime = control.now();
    fine = false;
    control.hide();
    control.freeze();
    send();
}

void PointerHold::moved(double dy, bool isFine, double timestamp)
{
    // An event from before the start: its motion is already in the travel read then.
    if (!held || timestamp <= startTime || (dy == 0 && isFine == fine)) {
        return;
    }
    travel += dy;
    fine = isFine;
    if (reporting > 0) {
        waiting = true;
        return;
    }
    send();
}

void PointerHold::flush()
{
    if (held && waiting) {
        send();
    }
}

void PointerHold::poll()
{
    if (held && !control.primaryButtonDown()) {
        end();
    }
}

void PointerHold::end()
{
    if (!held) {
        return;
    }
    held = false;
    // Reports still on their way belong to this hold: their completions are ignored.
    *alive = false;
    alive = std::make_shared<bool>(true);
    reporting = 0;
    waiting = false;
    const ScreenPoint at = control.position();
    if (std::abs(at.x - heldPress.x) >= kWarpDistance ||
        std::abs(at.y - heldPress.y) >= kWarpDistance) {
        control.warp(heldPress);
    }
    control.unfreeze();
    control.show();
}

void PointerHold::send()
{
    waiting = false;
    ++reporting;
    report(travel, fine, [this, current = std::weak_ptr<bool>(alive)] {
        const auto still = current.lock();
        if (!still || !*still) {
            return;
        }
        --reporting;
        if (reporting == 0 && waiting) {
            send();
        }
    });
}

} // namespace orizon::editor
