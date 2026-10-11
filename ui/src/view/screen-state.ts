// What the screen shows, decided from the settings and the latest frame: which channel the
// readouts measure, where the trigger marker sits, the view toggles' states, and how far a frozen
// view pans.
import { ChannelView, ViewLayout } from "../../../protocol/src/enums.ts";

export const viewOrder = ["left", "right", "mid", "side"] as const;
export type ViewName = (typeof viewOrder)[number];

// The channel the readouts, the hover and snapping measure when views are overlaid: the trigger
// source, or the Mid in Beats mode, where the trigger does not apply.
export function readoutView(
  beats: boolean,
  triggerSource: ViewName,
): ChannelView {
  return beats ? ChannelView.mid : ChannelView[triggerSource];
}

// How far back a frozen Seconds view is panned: from the freeze (0) to as far as the engine holds
// audio for this span (`reach`, which shrinks when the span grows).
export function clampPan(seconds: number, reach: number): number {
  return Math.min(reach, Math.max(0, seconds));
}

// The lane the trigger level marker sits in. Overlaid, the one lane. Stacked: in the multi-track
// view the own track's lane (`ownLane`, −1 when not found: the first), as the own track
// triggers; otherwise the trigger source's lane among those drawn, or the first lane when it is
// not drawn (a mono track triggers on L).
export function markerLane(
  layout: ViewLayout,
  multiTrack: boolean,
  ownLane: number,
  drawn: readonly ViewName[],
  triggerSource: ViewName,
): number {
  if (layout !== ViewLayout.stacked) {
    return 0;
  }
  return Math.max(0, multiTrack ? ownLane : drawn.indexOf(triggerSource));
}

// A view toggle: pressed when the view is shown; disabled in the multi-track view, where each lane
// is a track's Mid, and, on a mono track, for all but Left.
export function viewToggleState(
  view: ViewName,
  shown: readonly ViewName[],
  multiTrack: boolean,
  inputChannels: number,
): { pressed: boolean; disabled: boolean } {
  return {
    pressed: shown.includes(view),
    disabled: multiTrack || (inputChannels < 2 && view !== "left"),
  };
}
