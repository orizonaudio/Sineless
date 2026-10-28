// The editor's native pointer hold, where the platform has one (the macOS editor, except where an
// Audio Unit is hosted out of process): while a value is dragged it hides the pointer and keeps
// it in place, so the drag goes on past the screen's edge, and on release shows it again where it
// was pressed. The editor defines window.orizonPointerHold; without it a drag follows the pointer.

export interface PointerHold {
  // A value was pressed: hide and hold the pointer if its press is still down. The editor
  // answers through onTravel, or not at all if it cannot hold it.
  start(): void;
  // The drag has ended: show the pointer again where it was pressed.
  end(): void;
  // The travel since the press in CSS px, down positive (as clientY counts), and whether Shift is
  // held. The first call comes as the hold starts.
  onTravel: ((travel: number, fine: boolean) => void) | undefined;
}

declare global {
  interface Window {
    orizonPointerHold?: PointerHold;
  }
}

// The editor's pointer hold for a drag that `down` starts: only a left-button mouse drag is held.
export function pointerHoldFor(down: PointerEvent): PointerHold | undefined {
  return down.pointerType === "mouse" && down.button === 0
    ? window.orizonPointerHold
    : undefined;
}
