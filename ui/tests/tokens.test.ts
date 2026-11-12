// The design's colour tokens (docs/ui/tokens/sineless-tokens.json, the source of truth) are the
// colours the page uses (ui/src/tokens.css): a colour changed in one and not the other would ship
// a look the design does not describe. Fonts, sizes and shadows take different forms in the two
// files and are left out.
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { expect, test } from "vitest";

const root = join(import.meta.dirname, "..", "..");
const json = JSON.parse(
  readFileSync(join(root, "docs/ui/tokens/sineless-tokens.json"), "utf8"),
) as { sl: { color: Record<string, { $value: string }> } };
const css = readFileSync(join(root, "ui/src/tokens.css"), "utf8");

type Rgba = [number, number, number, number];

// "#rrggbb", "#rrggbbaa" or "rgb(r g b / a%)", as the two files write them.
function rgba(value: string): Rgba {
  const hex = /^#([0-9a-f]{6})([0-9a-f]{2})?$/iu.exec(value.trim());
  if (hex) {
    const digits = hex[1] ?? "";
    const n = (i: number) => parseInt(digits.slice(i, i + 2), 16);
    return [
      n(0),
      n(2),
      n(4),
      hex[2] === undefined ? 1 : parseInt(hex[2], 16) / 255,
    ];
  }
  const rgb = /^rgb\((\d+) (\d+) (\d+) \/ ([\d.]+)%\)$/u.exec(value.trim());
  if (rgb) {
    return [
      Number(rgb[1]),
      Number(rgb[2]),
      Number(rgb[3]),
      Number(rgb[4]) / 100,
    ];
  }
  throw new Error(`not a colour: ${value}`);
}

function cssToken(name: string): string | undefined {
  return new RegExp(`--sl-${name}:\\s*([^;]+);`, "u").exec(css)?.[1];
}

test.each(Object.entries(json.sl.color))(
  "the page draws the design's %s colour",
  (name, token) => {
    const page = cssToken(name);
    expect(page, `--sl-${name} in tokens.css`).toBeDefined();
    const [r, g, b, a] = rgba(page ?? "");
    const [dr, dg, db, da] = rgba(token.$value);
    expect([r, g, b]).toEqual([dr, dg, db]);
    // An 8-digit hex alpha is in 255ths, a CSS percentage in hundredths.
    expect(Math.abs(a - da)).toBeLessThanOrEqual(1 / 255);
  },
);

// The chrome is one material (#409): its greys share one hue, red and green equal and blue above
// them, the lighter ones carrying more blue, so the tint holds from the bar up to the light "on"
// fill rather than fading out of the lighter greys.
const chromeGreys = [
  "case",
  "control",
  "control-hover",
  "control-open",
  "popover",
  "line",
  "selected",
  "ink-2",
  "screen",
  "grid",
  "grid-strong",
  "lane-edge",
];

test("the chrome's greys share one hue, the lighter ones bluer", () => {
  const greys = chromeGreys
    .map((name) => {
      const [r, g, b] = rgba(cssToken(name) ?? "");
      return { name, redGreen: g - r, blue: b - r, light: r };
    })
    .sort((a, b) => a.light - b.light);
  for (const grey of greys) {
    expect(grey.redGreen, grey.name).toBe(0);
  }
  for (const [i, grey] of greys.slice(1).entries()) {
    expect(grey.blue, grey.name).toBeGreaterThanOrEqual(greys[i]?.blue ?? NaN);
  }
  expect(greys.at(-1)?.blue).toBeGreaterThan(greys[0]?.blue ?? NaN);
});
