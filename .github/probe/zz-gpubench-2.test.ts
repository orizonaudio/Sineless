// Throwaway: the worst Compare frame (16 overlaid full-scale lanes) drawn on the GPU, timed.
import { test } from "vitest";
import { ChannelView, FrameMode, ViewLayout } from "../../../protocol/src/enums.ts";
import { decodeFrame } from "../../../protocol/src/frame-decoder.ts";
import { encodeFrame } from "../../../protocol/src/frame-encoder.ts";
import { TraceRenderer } from "../../src/render/trace-renderer.ts";

test.each([[1382, 600], [2764, 1200]])("gpu %i", (width, height) => {
  const columns = width;
  const lanes = Array.from({ length: 16 }, (_, lane) => {
    const v: number[] = [];
    for (let c = 0; c < columns; c++) {
      const a = Math.sin(c * 0.37 + lane) * 0.85 + Math.sin(c * 1.7 + lane * 2) * 0.1;
      v.push(a - 0.08, a + 0.08);
    }
    return v;
  });
  const frame = decodeFrame(encodeFrame({
    mode: FrameMode.minMax, pointCount: columns, lanes, viewStart: 0, viewEnd: 1,
    triggered: false, frozen: false, roll: false, startFraction: 0, peak: 0, rms: 0,
    frequency: undefined, sampleRate: 48000, views: new Map(),
  }));
  const canvas = document.createElement("canvas");
  canvas.width = width;
  canvas.height = height;
  const colors = Array.from({ length: 16 }, (_, k) => [k / 16, 0.5, 1 - k / 16, 1] as const);
  const renderer = new TraceRenderer(canvas, {
    colors: () => new Map(Object.values(ChannelView).map((v) => [v, [1, 1, 1, 1] as const])),
    laneColors: () => colors,
    comparedLanes: () => [],
    sweepColors: () => ({ cursor: [1, 1, 1, 1], previous: [0, 0, 0, 0.5] }),
    cancellationColor: () => [1, 0, 0, 0.16],
    lineWidthPixels: 3,
  }, () => ({ verticalScaleDb: 0, layout: ViewLayout.overlaid }));
  const gl = canvas.getContext("webgl2") as WebGL2RenderingContext;
  const pixel = new Uint8Array(4);
  const once = () => { renderer.draw(frame); gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, pixel); };
  for (let i = 0; i < 10; i++) once();
  const times: number[] = [];
  for (let i = 0; i < 60; i++) { const t0 = performance.now(); once(); times.push(performance.now() - t0); }
  times.sort((a, b) => a - b);
  throw new Error("RESULT " + JSON.stringify({ size: width, median: Math.round(times[30]! * 100) / 100, p10: Math.round(times[6]! * 100) / 100 }));
});
