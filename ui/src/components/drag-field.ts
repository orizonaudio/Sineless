// A numeric field: drag vertically or scroll to step, double-click to type; where the editor can,
// a drag holds the pointer (pointer-hold.ts). It shows only values set with show(), i.e. values
// the engine reported; requests go through onRequest.
// attach() extends the same to an area around it, such as the pill that holds its icon.
import { followDrag } from "./follow-drag.ts";
import { pointerHoldFor } from "./pointer-hold.ts";
import { WheelSteps, wheelUnit } from "./wheel-steps.ts";

export interface DragFieldOptions {
  readonly label: string;
  readonly format: (value: number) => string;
  readonly parse: (text: string) => number | undefined;
  // The value `delta` steps away from `value`; `fine` when Shift is held.
  readonly step: (value: number, delta: number, fine: boolean) => number;
  readonly onRequest: (value: number) => void;
}

const pixelsPerStep = 8;

export class DragField {
  readonly element: HTMLElement;
  private readonly options: DragFieldOptions;
  private readonly display: HTMLSpanElement;
  private readonly wheel = new WheelSteps();
  private value = 0;
  // The value requested during a drag, so steps accumulate before the engine answers.
  private dragValue = 0;

  constructor(options: DragFieldOptions) {
    this.options = options;
    this.element = document.createElement("div");
    this.element.className = "field";
    this.element.tabIndex = 0;
    this.element.setAttribute("role", "spinbutton");
    this.element.setAttribute("aria-label", options.label);
    this.display = document.createElement("span");
    this.element.append(this.display);
    this.listen();
  }

  show(value: number): void {
    this.value = value;
    if (this.element.querySelector("input") === null) {
      this.display.textContent = this.options.format(value);
    }
    this.element.setAttribute("aria-valuetext", this.options.format(value));
  }

  private request(value: number): void {
    this.dragValue = value;
    this.options.onRequest(value);
  }

  // Makes `area`, which holds this field, act as the field too: a press, the wheel or a
  // double-click anywhere on it, its icon included, does what it does on the value. Nothing
  // happens while the field is hidden, so an area may hold several fields, one shown at a time.
  attach(area: HTMLElement): void {
    const outside = (event: Event) =>
      !this.element.contains(event.target as Node) &&
      this.element.offsetParent !== null;
    area.addEventListener("pointerdown", (event) => {
      // While the value is being typed, the press only ends the typing, as it does on the value:
      // the input's blur commits it.
      if (outside(event) && this.element.querySelector("input") === null) {
        // The value takes focus, not the body the icon's mousedown would give it.
        event.preventDefault();
        this.element.focus();
        this.drag(event);
      }
    });
    area.addEventListener(
      "wheel",
      (event) => {
        if (outside(event)) {
          this.scroll(event);
        }
      },
      { passive: false },
    );
    area.addEventListener("dblclick", (event) => {
      if (outside(event)) {
        this.edit();
      }
    });
  }

  private listen(): void {
    this.element.addEventListener("pointerdown", (event) => {
      this.drag(event);
    });
    this.element.addEventListener(
      "wheel",
      (event) => {
        this.scroll(event);
      },
      { passive: false },
    );
    this.element.addEventListener("dblclick", () => {
      this.edit();
    });
  }

  private drag(event: PointerEvent): void {
    if (this.element.querySelector("input") !== null) {
      return;
    }
    const startY = event.clientY;
    let applied = 0;
    const from = this.value;
    // Steps for `travel` up from the press, in CSS px.
    const travelTo = (travel: number, fine: boolean) => {
      const steps = Math.trunc(travel / pixelsPerStep) - applied;
      if (steps !== 0) {
        applied += steps;
        this.request(this.options.step(this.dragValue, steps, fine));
      }
    };
    // From the press the editor, where it can, holds the pointer and reports its travel instead.
    // Both count from the press, so switching neither loses nor repeats a step; until the editor
    // reports, the pointer's own travel goes on counting.
    const hold = pointerHoldFor(event);
    let reported = false;
    let ended = false;
    const onTravel = (travel: number, fine: boolean) => {
      if (ended) {
        return; // Sent before this drag ended, arriving after.
      }
      reported = true;
      travelTo(-travel, fine);
    };
    if (
      followDrag(
        this.element,
        event,
        (moveEvent) => {
          if (reported) {
            return; // The held pointer stays where it is.
          }
          travelTo(startY - moveEvent.clientY, moveEvent.shiftKey);
        },
        () => {
          ended = true;
          if (hold !== undefined) {
            if (hold.onTravel === onTravel) {
              hold.onTravel = undefined;
            }
            hold.end();
          }
        },
      )
    ) {
      this.dragValue = from;
      if (hold !== undefined) {
        hold.onTravel = onTravel;
        hold.start();
      }
    }
  }

  private scroll(event: WheelEvent): void {
    event.preventDefault();
    const unit = wheelUnit(event, window.innerHeight);
    // macOS turns Shift + wheel (fine steps) into horizontal scrolling.
    const delta =
      event.deltaY === 0 && event.shiftKey ? event.deltaX : event.deltaY;
    const steps = this.wheel.add(delta * unit, event.timeStamp);
    if (steps !== 0) {
      // Within a stream, from the value already asked for: the engine may not have
      // answered yet.
      const from = this.wheel.continues ? this.dragValue : this.value;
      this.request(this.options.step(from, steps, event.shiftKey));
    }
  }

  private edit(): void {
    const input = document.createElement("input");
    input.value = this.options.format(this.value);
    this.display.replaceChildren(input);
    input.focus();
    input.select();
    let done = false;
    const finish = (commit: boolean) => {
      if (done) {
        return;
      }
      done = true;
      const parsed = commit ? this.options.parse(input.value) : undefined;
      input.remove();
      this.display.textContent = this.options.format(this.value);
      if (parsed !== undefined) {
        this.request(parsed);
      }
    };
    input.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        finish(true);
      } else if (event.key === "Escape") {
        finish(false);
      }
    });
    input.addEventListener("blur", () => {
      finish(true);
    });
  }
}
