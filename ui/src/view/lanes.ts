// Multi-track lanes: what each lane is called and drawn in. A track without a host name is
// "Track n", and one without a host colour takes the fallback palette's nth colour, n its place
// among every Sineless instance in the host's order: the same in the Views popover's list of
// tracks and on the screen, whichever tracks are shown.
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
  // Its number among the instances on its track, as the engine counts them; 0 for the only one.
  readonly instance: number;
}

// Several instances on one host track (one before an effect and one after it) carry the track's
// name and colour: each lane then shows its number after the name, and from the second on a
// shade of the colour. Lanes without a host colour already differ, by the palette.
// `places` gives each track's place in the list of every instance, by id; a track not in it (the
// list has not come yet) takes its place among `labels`.
export function laneStyles(
  labels: readonly LaneLabel[],
  places?: ReadonlyMap<number, number>,
): LaneStyle[] {
  return labels.map((label, index) => {
    const place = places?.get(label.id) ?? index;
    return {
      id: label.id,
      name: label.name === "" ? `Track ${String(place + 1)}` : label.name,
      colour:
        label.colour === null
          ? (fallbackPalette[place % fallbackPalette.length] ?? 0xffffff)
          : shade(label.colour, label.instance),
      own: label.own,
      instance: label.instance,
    };
  });
}

// A lane's name as shown and as said: the track's name, then its number among the track's
// instances when it has one ("Bass 2" with a small lowered 1; "Bass 2, instance 1" to a screen
// reader, so the number is not taken for part of the name). Every place a lane is named uses it.
export function laneName(lane: {
  readonly name: string;
  readonly instance: number;
}): {
  readonly name: string;
  readonly number: string | undefined;
  readonly spoken: string | undefined;
} {
  return lane.instance > 0
    ? {
        name: lane.name,
        number: String(lane.instance),
        spoken: `, instance ${String(lane.instance)}`,
      }
    : { name: lane.name, number: undefined, spoken: undefined };
}

// The screen's colour, which every trace is drawn on (--sl-screen in tokens.css; a test keeps
// the two the same).
export const screenColour = 0x030307;
// The least contrast a shade keeps against the screen: the bar docs/ui/colour.html sets for
// traces, thin lines held to what WCAG 2.2 AA asks of text.
export const shadeContrast = 4.5;
// How far apart the shades of one colour are, in OKLab lightness (0 to 1): a step the eye
// separates at a glance on a thin line.
const shadeStep = 0.1;

type Triple = readonly [number, number, number];

function channels(colour: number): Triple {
  return [(colour >> 16) & 0xff, (colour >> 8) & 0xff, colour & 0xff];
}

function toLinear(channel: number): number {
  const c = channel / 255;
  return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
}

function fromLinear(value: number): number {
  const c = Math.min(1, Math.max(0, value));
  const encoded = c <= 0.0031308 ? c * 12.92 : 1.055 * c ** (1 / 2.4) - 0.055;
  return Math.round(encoded * 255);
}

// Relative luminance and contrast ratio, as WCAG defines them.
function luminance(colour: number): number {
  const [r, g, b] = channels(colour).map(toLinear) as unknown as Triple;
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

export function contrast(first: number, second: number): number {
  const [x, y] = [luminance(first), luminance(second)];
  return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05);
}

// Linear sRGB to OKLab and back, with Björn Ottosson's coefficients (bottosson.github.io/posts/
// oklab, public domain): a lightness that steps evenly to the eye, with the hue kept as it changes.
function toOklab(colour: number): Triple {
  const [r, g, b] = channels(colour).map(toLinear) as unknown as Triple;
  const l = Math.cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
  const m = Math.cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
  const s = Math.cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
  return [
    0.2104542553 * l + 0.793617785 * m - 0.0040720468 * s,
    1.9779984951 * l - 2.428592205 * m + 0.4505937099 * s,
    0.0259040371 * l + 0.7827717662 * m - 0.808675766 * s,
  ];
}

function fromOklab([lightness, a, b]: Triple): number {
  const l = (lightness + 0.3963377774 * a + 0.2158037573 * b) ** 3;
  const m = (lightness - 0.1055613458 * a - 0.0638541728 * b) ** 3;
  const s = (lightness - 0.0894841775 * a - 1.291485548 * b) ** 3;
  const red = fromLinear(
    4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
  );
  const green = fromLinear(
    -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
  );
  const blue = fromLinear(
    -0.0041960863 * l - 0.7034186147 * m + 1.707614701 * s,
  );
  return (red << 16) | (green << 8) | blue;
}

// The colour of a track's `instance`-th lane: the track's own for its only or first instance,
// then shades of it, alternately lighter and darker by a step more each time, leaving out any
// that would leave the range of lightness or fall under the contrast traces keep against the
// screen (a dark colour's shades are all lighter, a light one's all darker). A track with more
// instances than it has such shades starts over at the first.
export function shade(colour: number, instance: number): number {
  if (instance <= 1) {
    return colour;
  }
  const [lightness, a, b] = toOklab(colour);
  const shades: number[] = [];
  for (let step = 1; step * shadeStep < 1; step += 1) {
    for (const sign of [1, -1]) {
      const stepped = lightness + sign * step * shadeStep;
      if (stepped <= 0 || stepped >= 1) {
        continue;
      }
      const candidate = fromOklab([stepped, a, b]);
      if (contrast(candidate, screenColour) >= shadeContrast) {
        shades.push(candidate);
      }
    }
  }
  return shades[(instance - 2) % Math.max(shades.length, 1)] ?? colour;
}

export function cssColour(colour: number): string {
  return `#${colour.toString(16).padStart(6, "0")}`;
}
