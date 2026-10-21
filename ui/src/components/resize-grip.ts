// The resize grip in the editor's bottom-right corner. Dragging it asks the plugin for the
// window size under the pointer: hosts that offer no edge of their own to drag (Logic, and
// REAPER for Audio Units) can only be resized from inside the plugin.
import { followDrag } from "./follow-drag.ts";

export function attachResizeGrip(
  element: HTMLElement,
  onResize: (width: number, height: number) => void,
): void {
  let asked: { width: number; height: number } | undefined;
  let scheduled = false;
  const send = () => {
    scheduled = false;
    if (asked !== undefined) {
      onResize(asked.width, asked.height);
      asked = undefined;
    }
  };
  element.addEventListener("pointerdown", (event) => {
    if (event.button !== 0) {
      return;
    }
    event.preventDefault();
    // Screen coordinates: the page grows under the pointer while it moves.
    const start = {
      x: event.screenX,
      y: event.screenY,
      width: window.innerWidth,
      height: window.innerHeight,
    };
    followDrag(
      element,
      event,
      (moveEvent) => {
        asked = {
          width: Math.round(start.width + moveEvent.screenX - start.x),
          height: Math.round(start.height + moveEvent.screenY - start.y),
        };
        // At most one request a frame: the host resizes its window for each.
        if (!scheduled) {
          scheduled = true;
          requestAnimationFrame(send);
        }
      },
      send,
    );
  });
}
