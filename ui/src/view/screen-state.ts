// What the screen shows, decided from the settings and the latest frame: which channel the
// readouts measure, where the trigger marker sits, the view toggles' states, and how far a frozen
// view pans.
import { ChannelView, ViewLayout } from "../../../protocol/src/enums.ts";

export const viewOrder = ["left", "right", "mid", "side"] as const;
export type ViewName = (typeof viewOrder)[number];

// Each channel view's name, as the Channel button and the hover show it.
export const viewNames: Record<ChannelView, string> = {
  [ChannelView.left]: "Left",
  [ChannelView.right]: "Right",
  [ChannelView.mid]: "Mid",
  [ChannelView.side]: "Side",
};

// The channel the Meter, and with the views overlaid the hover and snapping, measure: the one the
// frame says its readouts measured. The engine decides it (the readout source when drawn, else
// the first view drawn; L on a mono track), so it is always a view the frame draws.
export function readoutView(frame: {
  readonly readoutView: ChannelView;
}): ChannelView {
  return frame.readoutView;
}

// How far back a frozen Seconds view is panned: from the freeze (0) to as far as the engine holds
// audio for this span (`reach`, which shrinks when the span grows).
export function clampPan(seconds: number, reach: number): number {
  return Math.min(reach, Math.max(0, seconds));
}

// The lane the trigger level marker sits in. Overlaid, the one lane. Stacked: in the multi-track
// view the own track's lane (`ownLane`, −1 when not found: the first), as the own track
// triggers; otherwise the lane of the view the trigger reads, which the frame names and draws (the
// first lane before a frame says, when the setting is not among those drawn).
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
// is a track's lane channel, and, on a mono track, for all but Left.
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
