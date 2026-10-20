// Follows a drag on a screen element from the pointerdown that starts it until it ends. The
// browser can end a drag without a pointerup: a touch or pen drag it takes for a pan sends
// pointercancel, and capture can be lost to another element. A drag left waiting for its
// pointerup would go on moving the cursor or marker under a pointer that only hovers, and
// apply its result on the next click.

const endings = ["pointerup", "pointercancel", "lostpointercapture"] as const;

// Calls `move` for each pointer move, then `end` once, with whether the button was released
// (false for a cancelled drag, whose result is not applied).
export function followDrag(
  element: HTMLElement,
  down: PointerEvent,
  move: (event: PointerEvent) => void,
  end: (released: boolean) => void = () => undefined,
): void {
  element.setPointerCapture(down.pointerId);
  const stop = (event: PointerEvent) => {
    element.removeEventListener("pointermove", move);
    for (const type of endings) {
      element.removeEventListener(type, stop);
    }
    end(event.type === "pointerup");
  };
  element.addEventListener("pointermove", move);
  for (const type of endings) {
    element.addEventListener(type, stop);
  }
}
