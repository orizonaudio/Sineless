// Multi-track lanes: what each lane is called and drawn in. A track without a host name is
// "Track n" by its lane position; one without a host colour takes the fallback palette.
import type { LaneLabel } from "../../../protocol/src/messages.ts";

// The Compare view's sum lane: a neutral colour no track gets from the palette.
export const sumLaneColour = 0xe6e2d8;

// One colour per lane up to the lane limit (16), none of them the Compare sum lane's neutral.
export const fallbackPalette = [
  0xe8b04b, 0x5ec8e5, 0xc48be0, 0x7fcf8f, 0xe8a35a, 0x7fa8e8, 0xf0708a,
  0xc8a070, 0x5fd3b8, 0xe0d060, 0xd86060, 0x9f7fe8, 0xb8e07a, 0xf0a080,
  0x80c8f0, 0xe080c0,
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
