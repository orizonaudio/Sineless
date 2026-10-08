import { describe, expect, test } from "vitest";

import { FrameMode } from "../../protocol/src/enums.ts";
import type { Frame } from "../../protocol/src/frame-decoder.ts";
import { RenderLoop } from "../src/render/render-loop.ts";

function frame(sequence: number): Frame {
  return {
    sequence,
    mode: FrameMode.raw,
    triggered: false,
    frozen: false,
    roll: false,
    pointCount: 0,
    samplesPerPoint: 1,
    startFraction: 0,
    peak: 0,
    rms: 0,
    frequency: undefined,
    synced: false,
    limited: false,
    cursor: undefined,
    cycleStart: 0,
    cycleSeconds: 0,
    spanQuarterNotes: 0,
    barQuarterNotes: 0,
    viewStart: 0,
    viewEnd: 1,
    visibleSamples: 0,
    views: new Map(),
    lanes: [],
    laneSet: 0,
    comparison: undefined,
  };
}

function setup() {
  const pending: ((timestamp: number) => void)[] = [];
  const drawn: number[] = [];
  let credits = 0;
  const loop = new RenderLoop(
    (callback) => pending.push(callback),
    { draw: (drawnFrame) => drawn.push(drawnFrame.sequence) },
    () => {
      credits++;
    },
  );
  const tick = () => {
    const callbacks = pending.splice(0);
    for (const callback of callbacks) {
      callback(0);
    }
  };
  return { loop, drawn, tick, credits: () => credits, pending };
}

describe("render loop", () => {
  test("draws engine frames no more often than the minimum interval", () => {
    const pending: ((timestamp: number) => void)[] = [];
    const drawn: number[] = [];
    const loop = new RenderLoop(
      (callback) => pending.push(callback),
      { draw: (drawnFrame) => drawn.push(drawnFrame.sequence) },
      () => undefined,
      15,
    );
    const tickAt = (timestamp: number) => {
      for (const callback of pending.splice(0)) {
        callback(timestamp);
      }
    };
    loop.submit(frame(1));
    tickAt(100);
    loop.submit(frame(2));
    tickAt(108); // 120 Hz: too soon.
    expect(drawn).toEqual([1]);
    expect(pending).toHaveLength(1);
    tickAt(116);
    expect(drawn).toEqual([1, 2]);
  });

  test("draws on the next tick, then returns the credit", () => {
    const { loop, drawn, tick, credits } = setup();
    loop.submit(frame(1));
    expect(drawn).toEqual([]);
    expect(credits()).toBe(0);
    tick();
    expect(drawn).toEqual([1]);
    expect(credits()).toBe(1);
  });

  test("draws only the newest of several frames per tick", () => {
    const { loop, drawn, tick, credits, pending } = setup();
    loop.submit(frame(1));
    loop.submit(frame(2));
    expect(pending).toHaveLength(1);
    tick();
    expect(drawn).toEqual([2]);
    expect(credits()).toBe(1);
  });

  test("does nothing and returns no credit while no frame arrives", () => {
    const { drawn, tick, credits } = setup();
    tick();
    expect(drawn).toEqual([]);
    expect(credits()).toBe(0);
  });

  test("redraws the last frame on request without earning a credit", () => {
    const { loop, drawn, tick, credits } = setup();
    loop.submit(frame(1));
    tick();
    loop.invalidate();
    tick();
    expect(drawn).toEqual([1, 1]);
    expect(credits()).toBe(1);
  });

  // While the page is hidden, requestAnimationFrame stops: no draws, no credits.
  test("waits for the clock while it is paused", () => {
    const { loop, drawn, credits } = setup();
    loop.submit(frame(1));
    loop.submit(frame(2));
    expect(drawn).toEqual([]);
    expect(credits()).toBe(0);
  });
});
