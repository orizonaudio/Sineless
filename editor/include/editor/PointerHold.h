#pragma once

#include <functional>
#include <memory>
#include <optional>

namespace orizon::editor {

// A point on the screen in the platform's global units, y down (Quartz's on macOS).
struct ScreenPoint
{
    double x = 0;
    double y = 0;
};

// The real mouse pointer, as a hold uses it; a fake in tests, which run in the developer's session.
class PointerControl
{
public:
    virtual ~PointerControl() = default;

    [[nodiscard]] virtual ScreenPoint position() = 0;
    [[nodiscard]] virtual bool primaryButtonDown() = 0;
    // Seconds, on the clock of the platform's mouse event timestamps.
    [[nodiscard]] virtual double now() = 0;
    virtual void hide() = 0;
    virtual void show() = 0;
    // While frozen the pointer stays put and mouse events carry only their motion.
    virtual void freeze() = 0;
    virtual void unfreeze() = 0;
    // Moves the pointer; called only while frozen.
    virtual void warp(ScreenPoint point) = 0;
};

// Holds the pointer during a value drag in the page, as Live's own devices do: from the press the
// pointer hides and stays put, the drag goes on past the screen's edge, and on
// release the pointer shows again where it was pressed. The page steps from the travel reported
// here, in CSS px down from the press; it is measured from the press, as the page's own travel
// is, so the page can switch from one to the other without losing or repeating a step.
class PointerHold
{
public:
    // Sends the travel to the page; `done` is called once it is delivered, or has failed.
    using Report = std::function<void(double travel, bool fine, std::function<void()> done)>;

    PointerHold(PointerControl& control, Report report);
    ~PointerHold();
    PointerHold(const PointerHold&) = delete;
    PointerHold& operator=(const PointerHold&) = delete;

    // A left-button press on the page.
    void pressed(ScreenPoint point);
    // A value was pressed: holds the pointer if that press is still down.
    void start();
    // A drag event's motion down, with Shift (fine steps), stamped on PointerControl::now's clock.
    void moved(double dy, bool fine, double timestamp);
    // Sends the latest travel now, even while a report is on its way.
    void flush();
    // Ends the hold if the button is up, in case its release was never seen.
    void poll();
    // Shows the pointer again where it was pressed.
    void end();

    [[nodiscard]] bool holding() const { return held; }

private:
    void send();

    PointerControl& control;
    Report report;
    std::optional<ScreenPoint> press; // Recorded, not yet held.
    ScreenPoint heldPress;
    bool held = false;
    double startTime = 0;
    double travel = 0;
    bool fine = false;
    // Reports on their way (two after a flush), and whether newer values wait for them.
    int reporting = 0;
    bool waiting = false;
    // Tells a report's completion whether its hold, or this object, is still there.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

} // namespace orizon::editor
