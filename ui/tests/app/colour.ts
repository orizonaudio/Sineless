// Colours as the page resolves them, and WCAG contrast between them, for tests of what a user can
// tell apart: a translucent fill is laid over what it sits on, as the screen shows it.

export type Rgb = [number, number, number];

// A computed colour ("rgb(r, g, b)" or "rgba(r, g, b, a)") laid over an opaque one.
export function over(colour: string, under: Rgb): Rgb {
  const [r, g, b, alpha = 1] = (/rgba?\(([^)]+)\)/u.exec(colour)?.[1] ?? "")
    .split(",")
    .map(Number);
  if (
    r === undefined ||
    g === undefined ||
    b === undefined ||
    Number.isNaN(b)
  ) {
    throw new Error(`not a computed colour: ${colour}`);
  }
  const mix = (top: number, bottom: number) =>
    top * alpha + bottom * (1 - alpha);
  return [mix(r, under[0]), mix(g, under[1]), mix(b, under[2])];
}

export function opaque(colour: string): Rgb {
  return over(colour, [0, 0, 0]);
}

function luminance([r, g, b]: Rgb): number {
  const linear = (channel: number) => {
    const c = channel / 255;
    return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
  };
  return 0.2126 * linear(r) + 0.7152 * linear(g) + 0.0722 * linear(b);
}

export function contrast(a: Rgb, b: Rgb): number {
  const [x, y] = [luminance(a), luminance(b)];
  return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05);
}

// The space between each element's right edge and the next one's left edge.
export function gaps(elements: readonly Element[]): number[] {
  const boxes = elements.map((element) => element.getBoundingClientRect());
  return boxes.slice(1).map((box, i) => box.left - (boxes[i]?.right ?? NaN));
}

// The page's bar colour, under the bars' controls.
export function caseColour(): Rgb {
  return opaque(getComputedStyle(document.body).backgroundColor);
}
