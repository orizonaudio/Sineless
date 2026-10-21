// Follows a drag on a screen element from the pointerdown that starts it until it ends. The
// browser can end a drag without a pointerup: a touch or pen drag it takes for a pan sends
// pointercancel, and capture can be lost to another element. A drag left waiting for its
// pointerup would go on moving the cursor or marker under a pointer that only hovers, and
// apply its result on the next click.

const endings = ["pointerup", "pointercancel", "lostpointercapture"] as const;

// The elements being dragged: one drag at a time each, so a second finger on a touch screen
// neither starts another nor moves this one.
const dragging = new WeakSet<HTMLElement>();

// Calls `move` for each move of the pointer that started the drag, then `end` once, with whether
// the button was released (false for a cancelled drag, whose result is not applied). Other
// pointers, such as a second finger on a touch screen, are not followed. Returns false, starting
// nothing, while the element is already being dragged.
export function followDrag(
  element: HTMLElement,
  down: PointerEvent,
  move: (event: PointerEvent) => void,
  end: (released: boolean) => void = () => undefined,
): boolean {
  if (dragging.has(element)) {
    return false;
  }
  element.setPointerCapture(down.pointerId);
  dragging.add(element);
  const follow = (event: PointerEvent) => {
    if (event.pointerId === down.pointerId) {
      move(event);
    }
  };
  const stop = (event: PointerEvent) => {
    if (event.pointerId !== down.pointerId) {
      return;
    }
    element.removeEventListener("pointermove", follow);
    for (const type of endings) {
      element.removeEventListener(type, stop);
    }
    dragging.delete(element);
    end(event.type === "pointerup");
  };
  element.addEventListener("pointermove", follow);
  for (const type of endings) {
    element.addEventListener(type, stop);
  }
  return true;
}
