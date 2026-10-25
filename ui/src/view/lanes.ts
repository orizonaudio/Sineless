// Multi-track lanes: what each lane is called and drawn in. A track without a host name is
// "Track n" by its lane position; one without a host colour takes the fallback palette.
import type { LaneLabel } from "../../../protocol/src/messages.ts";

// The Compare view's sum lane: white, which no track gets from the palette (--sl-mid).
export const sumLaneColour = 0xf5f5f7;

// For tracks without a host colour (every Audio Unit host): the redesign's 16-colour track
// palette (docs/ui/tokens/, step 4), in its order, which keeps neighbouring lanes distinct with
// colour blindness too. None is near the orange accent or the sum's white.
export const fallbackPalette = [
  0x6fb6fe, 0xe3636c, 0x13efe9, 0x8f7fed, 0xe7a203, 0x0bab83, 0xdcd848,
  0xc76ac0, 0x98c244, 0xfeb8e3, 0x8bec8b, 0xf485cd, 0x879f03, 0xe6c0fe,
  0x42ce85, 0xe28bea,
] as const;

export interface LaneStyle {
  readonly id: number;
  readonly name: string;
  readonly colour: number; // 0xRRGGBB
  readonly own: boolean;
  readonly compared: boolean; // Compare view: in the sum, measured against this track.
}

export function laneStyles(labels: readonly LaneLabel[]): LaneStyle[] {
  return labels.map((label, index) => ({
    id: label.id,
    name: label.name === "" ? `Track ${String(index + 1)}` : label.name,
    colour:
      label.colour ??
      fallbackPalette[index % fallbackPalette.length] ??
      0xffffff,
    own: label.own,
    compared: label.compared,
  }));
}

export function cssColour(colour: number): string {
  return `#${colour.toString(16).padStart(6, "0")}`;
}
