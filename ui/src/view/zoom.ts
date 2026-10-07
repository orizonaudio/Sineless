// View zoom: which part of the frame the screen shows, as fractions of the frame's width.
// Temporary display state only; it never changes a setting.

export interface ViewWindow {
  readonly start: number;
  readonly end: number;
}

export const fullView: ViewWindow = { start: 0, end: 1 };

// The narrowest view: 256 times magnified.
export const minimumWidth = 1 / 256;

function width(view: ViewWindow): number {
  return view.end - view.start;
}

// Clamps a view of `size` starting at `start` into the frame.
function placed(start: number, size: number): ViewWindow {
  const clampedSize = Math.min(1, Math.max(minimumWidth, size));
  const clampedStart = Math.min(1 - clampedSize, Math.max(0, start));
  return { start: clampedStart, end: clampedStart + clampedSize };
}

export function isZoomed(view: ViewWindow): boolean {
  return width(view) < 1;
}

export function zoomFactor(view: ViewWindow): number {
  return 1 / width(view);
}

// Frame position (0 to 1) of a screen position (0 to 1), and back.
export function toFrame(view: ViewWindow, screen: number): number {
  return view.start + screen * width(view);
}

export function toScreen(view: ViewWindow, frame: number): number {
  return (frame - view.start) / width(view);
}

// Magnifies by `factor` (above 1 zooms in), keeping the frame position under `anchor` (a screen
// position) where it is.
export function zoomAround(
  view: ViewWindow,
  anchor: number,
  factor: number,
): ViewWindow {
  const pinned = toFrame(view, anchor);
  const size = Math.min(1, Math.max(minimumWidth, width(view) / factor));
  return placed(pinned - anchor * size, size);
}

// Moves the view by `screenDistance` screen widths (positive moves towards the frame's end).
export function panBy(view: ViewWindow, screenDistance: number): ViewWindow {
  return placed(view.start + screenDistance * width(view), width(view));
}

// Shows exactly the region between two screen positions.
export function zoomTo(view: ViewWindow, from: number, to: number): ViewWindow {
  const start = toFrame(view, Math.min(from, to));
  const end = toFrame(view, Math.max(from, to));
  const size = Math.max(minimumWidth, end - start);
  return placed((start + end) / 2 - size / 2, size);
}
