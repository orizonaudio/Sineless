// A numeric field: drag vertically or scroll to step, double-click to type. It shows
// only values set with show(), i.e. values the engine reported; requests go through onRequest.

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
    this.element.title = "Drag or scroll to change; double-click to type";
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

  private listen(): void {
    let startY = 0;
    let applied = 0;
    let dragging = false;
    this.element.addEventListener("pointerdown", (event) => {
      if (this.element.querySelector("input") !== null) {
        return;
      }
      dragging = true;
      startY = event.clientY;
      applied = 0;
      this.dragValue = this.value;
      this.element.setPointerCapture(event.pointerId);
    });
    this.element.addEventListener("pointermove", (event) => {
      if (!dragging) {
        return;
      }
      const steps =
        Math.trunc((startY - event.clientY) / pixelsPerStep) - applied;
      if (steps !== 0) {
        applied += steps;
        this.request(this.options.step(this.dragValue, steps, event.shiftKey));
      }
    });
    const stop = () => {
      dragging = false;
    };
    this.element.addEventListener("pointerup", stop);
    this.element.addEventListener("pointercancel", stop);
    this.element.addEventListener(
      "wheel",
      (event) => {
        event.preventDefault();
        this.request(
          this.options.step(
            this.value,
            event.deltaY < 0 ? 1 : -1,
            event.shiftKey,
          ),
        );
      },
      { passive: false },
    );
    this.element.addEventListener("dblclick", () => {
      this.edit();
    });
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
