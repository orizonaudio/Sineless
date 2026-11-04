// The frame barcode on the page's own traces (WebGL mode), turned on by the editor's snapshot
// (frameBarcode, the diagnostic switch): each picture carries its number and the time it was drawn
// for, read back here with the same reader the recording scripts use; off, there is no strip. While
// it is on, the cursor readout gives the strip its corner, and nothing else the page shows lies
// over the strip.
import { afterEach, describe, expect, test, vi } from "vitest";
import { userEvent } from "vitest/browser";

import {
  barcodeDataBits,
  barcodeTicksPerMillisecond,
} from "../../../protocol/src/limits.ts";
import {
  barcodeRowCells,
  barcodeStrip,
  decodeStrip,
  type StripRead,
} from "../../src/render/frame-barcode.ts";
import { TraceRenderer } from "../../src/render/trace-renderer.ts";
import { byId, inTimeBase, settle, startApp } from "./harness.ts";

const range = 2 ** barcodeDataBits;

// Each strip read from the trace canvas right after a draw, at its cells' centres; `onRead` runs
// with each read.
function readStrips(onRead?: () => void): StripRead[] {
  const reads: StripRead[] = [];
  const draw = Object.getOwnPropertyDescriptor(TraceRenderer.prototype, "draw")
    ?.value as (this: TraceRenderer, ...args: unknown[]) => void;
  vi.spyOn(TraceRenderer.prototype, "draw").mockImplementation(function (
    this: TraceRenderer,
    ...args: unknown[]
  ) {
    draw.apply(this, args);
    const canvas = byId("trace") as HTMLCanvasElement;
    // Only the page under test: an earlier test's page may still be drawing its own canvas.
    if ((this as unknown as { canvas: HTMLCanvasElement }).canvas !== canvas) {
      return;
    }
    const gl = canvas.getContext("webgl2");
    if (gl === null) {
      return;
    }
    const strip = barcodeStrip(
      canvas.width,
      canvas.height,
      window.devicePixelRatio,
    );
    const row = (index: number) =>
      Array.from({ length: barcodeRowCells }, (_, cell) => {
        const x = Math.floor(strip.left + (cell + 0.5) * strip.cell);
        const y = Math.floor(strip.top + (index + 0.5) * strip.cell);
        const pixel = new Uint8Array(4);
        gl.readPixels(
          x,
          canvas.height - 1 - y,
          1,
          1,
          gl.RGBA,
          gl.UNSIGNED_BYTE,
          pixel,
        );
        return ((pixel[0] ?? 0) + (pixel[1] ?? 0) + (pixel[2] ?? 0)) / 3 / 255;
      });
    reads.push(decodeStrip(row(0), row(1)));
    onRead?.();
  });
  return reads;
}

function expectSteppingStrips(reads: readonly StripRead[]): void {
  const decoded = reads.filter(
    (read): read is { picture: number; drawnFor: number } => "picture" in read,
  );
  expect(decoded.length).toBeGreaterThan(5);
  expect(decoded).toHaveLength(reads.length);
  for (let index = 1; index < decoded.length; index++) {
    const previous = decoded[index - 1];
    const current = decoded[index];
    if (previous === undefined || current === undefined) {
      continue;
    }
    expect((current.picture - previous.picture + range) % range).toBe(1);
    // Drawn for a later animation frame, by less than a second.
    const step = (current.drawnFor - previous.drawnFor + range) % range;
    expect(step).toBeGreaterThan(0);
    expect(step).toBeLessThan(1000 * barcodeTicksPerMillisecond);
  }
}

afterEach(() => {
  vi.restoreAllMocks();
});

describe("the page's strip", () => {
  test.each(["Beats", "Sec"])(
    "with the snapshot's switch, every picture carries its number and time, in %s",
    async (timeBase) => {
      const reads = readStrips();
      await startApp({ frameBarcode: true });
      await settle();
      if (timeBase === "Sec") {
        await inTimeBase("Sec");
      }
      reads.length = 0;
      await settle(12);
      expectSteppingStrips(reads);
    },
  );

  test("on a Beats frame before the host gives a tempo, which carries nothing to draw", async () => {
    const reads = readStrips();
    await startApp({ frameBarcode: true, hostTempo: false });
    await settle();
    reads.length = 0; // The page before this one finishes its queued draws as this one starts.
    await settle(12);
    expectSteppingStrips(reads);
  });

  test("in the browser preview, ?barcode turns it on without the snapshot's switch", async () => {
    const address = window.location.href;
    window.history.replaceState(null, "", "?barcode");
    try {
      const reads = readStrips();
      await startApp();
      await settle();
      reads.length = 0;
      await settle(12);
      expectSteppingStrips(reads);
    } finally {
      window.history.replaceState(null, "", address);
    }
  });

  // Lateness is the screen's time less the drawn-for time, so the row must carry the animation
  // frame's timestamp, not the time the draw ran. Timestamps shifted 5 s ahead tell them apart.
  test("each picture is drawn for its animation frame's time", async () => {
    const shift = 5000;
    let tickTime = Number.NaN;
    const request = window.requestAnimationFrame.bind(window);
    vi.spyOn(window, "requestAnimationFrame").mockImplementation((callback) =>
      request((timestamp) => {
        tickTime = timestamp + shift;
        callback(tickTime);
      }),
    );
    const drawnFor: number[] = [];
    const reads = readStrips(() => {
      drawnFor.push(Math.round(tickTime * barcodeTicksPerMillisecond) % range);
    });
    await startApp({ frameBarcode: true });
    await settle();
    reads.length = 0;
    drawnFor.length = 0;
    await settle(12);
    expectSteppingStrips(reads);
    expect(
      reads.map((read) => ("drawnFor" in read ? read.drawnFor : Number.NaN)),
    ).toEqual(drawnFor);
  });

  test("without the switch there is no strip", async () => {
    const reads = readStrips();
    await startApp();
    await settle();
    reads.length = 0;
    await settle(8);
    expect(reads.length).toBeGreaterThan(0);
    expect(reads.every((read) => "unreadable" in read)).toBe(true);
  });
});

describe("the strip's corner", () => {
  const cornerShown = () =>
    getComputedStyle(document.querySelector(".screen-corner") as HTMLElement)
      .display !== "none";

  test("the cursor readout gives way while the switch is on, and only then", async () => {
    await startApp({ frameBarcode: true });
    await settle();
    expect(cornerShown()).toBe(false);
    await startApp();
    await settle();
    expect(cornerShown()).toBe(true);
  });

  test("nothing the page shows lies over the strip, cursors on", async () => {
    await startApp({ frameBarcode: true });
    await settle();
    for (const id of ["time-cursors", "level-cursors"]) {
      await userEvent.click(byId(id));
    }
    await settle();
    const canvas = byId("trace") as HTMLCanvasElement;
    const box = canvas.getBoundingClientRect();
    const ratio = window.devicePixelRatio;
    const strip = barcodeStrip(canvas.width, canvas.height, ratio);
    const stripBox = {
      left: box.left + strip.left / ratio,
      top: box.top + strip.top / ratio,
      right: box.left + (strip.left + barcodeRowCells * strip.cell) / ratio,
      bottom: box.top + (strip.top + 2 * strip.cell) / ratio,
    };
    const overlays = [
      ...document.querySelectorAll<HTMLElement>(
        ".axis-label, .lane-label, .level-cursor span, .trigger-marker, .time-cursor span",
      ),
    ].filter((element) => element.offsetParent !== null);
    expect(overlays.length).toBeGreaterThan(0);
    for (const overlay of overlays) {
      const rect = overlay.getBoundingClientRect();
      const meets =
        rect.left < stripBox.right &&
        rect.right > stripBox.left &&
        rect.top < stripBox.bottom &&
        rect.bottom > stripBox.top;
      expect(meets, `${overlay.className} "${overlay.textContent}"`).toBe(
        false,
      );
    }
  });
});
