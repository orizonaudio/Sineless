// The difference's lane through the real page: filled in the page's own colours for what this
// track lost and what it gained, and measured against this track's lane wherever the host lists
// the track. The pixels are read straight after a draw.
import { describe, expect, test, vi } from "vitest";

import { ViewLayout } from "../../../protocol/src/enums.ts";
import type { Frame } from "../../../protocol/src/frame-decoder.ts";
import { traceLanes } from "../../src/render/trace-geometry.ts";
import { TraceRenderer } from "../../src/render/trace-renderer.ts";
import {
  beforeApp,
  byId,
  choice,
  press,
  setCompare,
  settle,
  showOnly,
  startApp,
} from "./harness.ts";

// How many pixels of the last lane are in the lost colour, and in the gained colour.
interface Filled {
  readonly lost: number;
  readonly gained: number;
}
let wanted = false;
let drawn: Filled | undefined;
let measuring = false;

// A design token's colour as the page reads it (a hex value), as bytes.
function tokenRgb(name: string): [number, number, number] {
  const hex = getComputedStyle(document.documentElement)
    .getPropertyValue(name)
    .trim()
    .slice(1);
  return [0, 2, 4].map((at) => Number.parseInt(hex.slice(at, at + 2), 16)) as [
    number,
    number,
    number,
  ];
}

// Measures the renderer's draws on request: reading the pixels back at every frame would slow
// the page.
function measureDraws(): void {
  if (measuring) {
    return;
  }
  measuring = true;
  const draw = Object.getOwnPropertyDescriptor(TraceRenderer.prototype, "draw")
    ?.value as (this: TraceRenderer, frame: Frame) => void;
  vi.spyOn(TraceRenderer.prototype, "draw").mockImplementation(function (
    this: TraceRenderer,
    frame: Frame,
  ) {
    draw.call(this, frame);
    if (!wanted) {
      return;
    }
    wanted = false;
    const canvas = byId("trace") as HTMLCanvasElement;
    const gl = canvas.getContext("webgl2");
    if (gl === null) {
      return;
    }
    const { width, height } = canvas;
    const pixels = new Uint8Array(width * height * 4);
    gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    const { center, halfHeight } = traceLanes(
      frame.lanes.length,
      ViewLayout.stacked,
      true,
    ).at(-1) ?? { center: 0, halfHeight: 1 };
    const bottom = Math.ceil(((center - halfHeight + 1) / 2) * height);
    const top = Math.floor(((center + halfHeight + 1) / 2) * height);
    // Fully covered pixels in a token's colour, within what blending rounds.
    const count = (token: string) => {
      const colour = tokenRgb(token);
      let found = 0;
      for (let at = bottom * width * 4; at < top * width * 4; at += 4) {
        if (
          (pixels[at + 3] ?? 0) >= 250 &&
          colour.every(
            (channel, index) =>
              Math.abs((pixels[at + index] ?? 0) - channel) <= 6,
          )
        ) {
          found++;
        }
      }
      return found;
    };
    drawn = { lost: count("--sl-lost"), gained: count("--sl-gained") };
  });
}

// The next frame the page draws, measured.
async function nextDraw(): Promise<Filled> {
  drawn = undefined;
  wanted = true;
  await vi.waitFor(
    () => {
      expect(drawn).toBeDefined();
    },
    { timeout: 5000 },
  );
  return drawn as unknown as Filled;
}

function lastLabel(): string {
  const labels = byId("lane-labels").querySelectorAll(".lane-label > span");
  return labels[labels.length - 1]?.textContent ?? "";
}

// Compare on the difference of the kick (this track) and the tracks named, stacked.
async function showDifference(others: readonly string[]): Promise<void> {
  await press(choice("time-base", "Trig"));
  await press(choice("display-mode", "Multi"));
  await press(choice("layout", "Stack"));
  await setCompare("Diff");
  await expect.poll(() => lastLabel().startsWith("Difference")).toBe(true);
  await showOnly(others);
  await settle();
}

describe("the difference's lane, with this track listed first", () => {
  beforeApp(async () => {
    measureDraws();
    await startApp();
  });

  // The kick against the bass, which has nothing in common with it: each reaches where the
  // other does not.
  test("is filled in the page's colours for what is lost and what is gained", async () => {
    await showDifference(["Bass"]);
    const { lost, gained } = await nextDraw();
    expect(lost, "lost").toBeGreaterThan(20);
    expect(gained, "gained").toBeGreaterThan(20);
  });
});

describe("the difference's lane, with this track listed after another", () => {
  beforeApp(async () => {
    measureDraws();
    // The host lists the bass before the kick, this track.
    await startApp({ tracksBefore: 1 });
  });

  // The kick against its second microphone lined up, at 0.8 of its level: the kick reaches
  // further everywhere, so it only loses. Read against another lane than the kick's, the fill
  // would show the kick's as gained.
  test("is filled against this track's lane, not the first", async () => {
    await showDifference(["Kick 2"]);
    const { lost, gained } = await nextDraw();
    expect(lost, "lost").toBeGreaterThan(20);
    expect(gained, "gained").toBe(0);
  });
});
