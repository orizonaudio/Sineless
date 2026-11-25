#include "editor/DelayReturn.h"

#include <algorithm>
#include <cmath>

namespace orizon::editor {

std::optional<int> refreshesAfterTarget(double presentedTime, double target, double periodSeconds)
{
    if (presentedTime <= 0.0 || periodSeconds <= 0.0) {
        return std::nullopt;
    }
    const double refreshes = std::floor((presentedTime - target) / periodSeconds + 0.5);
    if (refreshes < 0.0) {
        return std::nullopt;
    }
    return static_cast<int>(std::min(refreshes, 1000.0));
}

void DelayReturn::reach(bool byReturn)
{
    held = 0.0;
    heldByReturn = byReturn;
    halvings = 0;
    wasLater = false;
    laterSince.reset();
}

void DelayReturn::take(const Landed& landed, double time)
{
    const bool later = landed.refreshes > kShortestRefreshesAfterTarget;
    if (judging) {
        const int before = *judging;
        judging.reset();
        if (landed.refreshes >= before) {
            // Nothing gained: a repeated picture for nothing. Twice the wait, as after a return
            // whose gain was lost: under a burst of load a picture late straight after the return
            // lands here too, and that is no sign of a view that can never do better.
            wait = std::min(2.0 * wait, kLongestWaitSeconds);
        }
        if (later) {
            // Still later, sooner or not: counted from here.
            wasLater = true;
            laterRefreshes = landed.refreshes;
            laterSince = time;
        } else {
            reach(true);
        }
        return;
    }
    if (!later) {
        if (wasLater) {
            reach(false); // By itself: a callback drew nothing.
        }
        return;
    }
    wasLater = true;
    laterRefreshes = landed.refreshes;
    if (laterSince) {
        return;
    }
    laterSince = time;
    if (held) {
        // The shorter delay is lost. Within kHeldSeconds of a return reaching it, the return did
        // not pay. Reached by itself, the flip is the machine's: a longer wait would save no
        // repeated picture, only keep the traces later once the machine is free again.
        if (heldByReturn && halvings == 0) {
            wait = std::min(2.0 * wait, kLongestWaitSeconds);
        }
        held.reset();
    }
}

void DelayReturn::drewNothing(std::uint64_t lastPicture)
{
    laterSince.reset();
    judging.reset();
    ignoreUpTo = lastPicture;
}

bool DelayReturn::callback(const Callback& callback)
{
    const double elapsed = lastTime ? std::max(0.0, callback.time - *lastTime) : 0.0;
    lastTime = callback.time;
    if (restarted) {
        restarted = false;
        ignoreUpTo = callback.lastPicture;
    }
    if (held && callback.drawing) {
        // Only callbacks that draw count: a frozen display earns nothing.
        *held += elapsed;
        while (*held >= static_cast<double>(halvings + 1) * kHeldSeconds) {
            wait = std::max(kFirstWaitSeconds, wait / 2.0);
            ++halvings;
        }
    }
    if (callback.newest && callback.newest->picture > ignoreUpTo &&
        callback.newest->picture > counted) {
        counted = callback.newest->picture;
        take(*callback.newest, callback.time);
    }
    if (!callback.drawing) {
        drewNothing(callback.lastPicture);
        return false;
    }
    if (!laterSince || judging || callback.time - *laterSince < wait) {
        return false;
    }
    judging = laterRefreshes;
    laterSince.reset();
    ignoreUpTo = callback.lastPicture;
    ++made;
    return true;
}

void DelayReturn::pause()
{
    restarted = true;
    lastTime.reset();
    wasLater = false;
    laterSince.reset();
    judging.reset();
    held.reset();
    heldByReturn = false;
    halvings = 0;
}

void DelayReturn::displayChanged()
{
    pause();
    wait = kFirstWaitSeconds;
}

} // namespace orizon::editor
