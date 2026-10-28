import { describe, expect, test } from "vitest";

import { ChannelView, FrameMode } from "../../protocol/src/enums.ts";
import { decodeFrame, type Frame } from "../../protocol/src/frame-decoder.ts";
import {
  frozenHoldSeconds,
  triggerPosition,
} from "../../protocol/src/limits.ts";
import {
  mockFundamental,
  mockSummingGain,
  MockTransport,
  type MockOptions,
} from "../src/bridge/mock-transport.ts";
import type { ControlMessage } from "../src/bridge/transport.ts";

function connect(options: MockOptions = {}): {
  transport: MockTransport;
  messages: ControlMessage[];
  frames: Frame[];
} {
  const transport = new MockTransport({
    now: () => 1.0,
    schedule: (work) => {
      work();
    },
    ...options,
  });
  const messages: ControlMessage[] = [];
  const frames: Frame[] = [];
  transport.onMessage((message) => {
    messages.push(message);
  });
  transport.onFrame((frame) => {
    frames.push(decodeFrame(frame));
  });
  transport.send({
    type: "hello",
    protocolVersion: 1,
    viewWidth: 400,
    pixelRatio: 2,
  });
  transport.requestFrame();
  return { transport, messages, frames };
}

// Beats is the default time base; these tests use seconds.
function useSeconds(transport: MockTransport): void {
  transport.send({ type: "setSetting", name: "timeBase", value: "seconds" });
}

describe("mock back end", () => {
  test("answers hello with a snapshot, and the first request with a frame", () => {
    const { messages, frames } = connect();
    expect(messages[0]).toMatchObject({
      type: "snapshot",
      sampleRate: 48000,
      inputChannels: 2,
    });
    expect(frames).toHaveLength(1);
  });

  test("sends one frame per request and none without", () => {
    const { transport, frames } = connect();
    transport.requestFrame();
    transport.requestFrame();
    expect(frames).toHaveLength(3);
  });

  test("synthesises frames in the real format for the selected views", () => {
    const { transport, frames } = connect();
    useSeconds(transport);
    transport.send({
      type: "setSetting",
      name: "channelViews",
      value: ["mid", "side"],
    });
    transport.send({ type: "setSetting", name: "timeSpan", value: 0.1 });
    transport.requestFrame();
    const frame = frames.at(-1);
    expect(frame?.mode).toBe(FrameMode.minMax);
    expect(frame?.pointCount).toBe(800);
    expect([...(frame?.views.keys() ?? [])]).toEqual([
      ChannelView.mid,
      ChannelView.side,
    ]);
    expect(frame?.frequency).toBe(mockFundamental);
    expect(frame?.triggered).toBe(true);
  });

  test("echoes the kept value for valid and invalid settings", () => {
    const { transport, messages } = connect();
    transport.send({
      type: "setSetting",
      name: "triggerMode",
      value: "normal",
    });
    transport.send({ type: "setSetting", name: "timeSpan", value: 99 });
    expect(
      messages.filter((message) => message.type === "stateChanged"),
    ).toEqual([
      { type: "stateChanged", name: "triggerMode", value: "normal" },
      { type: "stateChanged", name: "timeSpan", value: 1 },
    ]);
  });

  test("draws the simulated group's tracks as lanes in Multi mode", () => {
    const { transport, messages, frames } = connect();
    const lanes = () => messages.filter((message) => message.type === "lanes");
    expect(lanes()).toHaveLength(1);
    expect(lanes()[0]).toMatchObject({ laneSet: 1, hidden: 0 });
    transport.send({ type: "setSetting", name: "displayMode", value: "multi" });
    transport.requestFrame();
    const frame = frames.at(-1);
    expect(frame?.views.size).toBe(0);
    expect(frame?.lanes).toHaveLength(4);
    expect(frame?.laneSet).toBe(1);

    transport.send({ type: "setSetting", name: "group", value: "b" });
    transport.requestFrame();
    expect(lanes()).toHaveLength(2);
    expect(lanes()[1]).toMatchObject({
      laneSet: 2,
      lanes: [{ name: "Kick", own: true }],
    });
    expect(frames.at(-1)?.lanes).toHaveLength(1);
  });

  test("compares this track with the checked tracks and previews the fix", () => {
    const { transport, messages, frames } = connect();
    transport.send({
      type: "setSetting",
      name: "displayMode",
      value: "compare",
    });
    transport.requestFrame();
    const lanes = messages.filter((message) => message.type === "lanes").at(-1);
    expect(lanes).toMatchObject({
      lanes: [
        { name: "Kick", compared: true },
        { name: "Bass", compared: true },
        { name: "", compared: false },
        { name: "Kick 2", compared: false },
      ],
    });
    expect(frames.at(-1)?.lanes).toHaveLength(5);
    expect(frames.at(-1)?.comparison?.matches).toHaveLength(4);
    // This track and those not compared have no match, as the engine sends.
    for (const index of [0, 2, 3]) {
      const none = frames.at(-1)?.comparison?.matches[index];
      expect(none?.correlation).toBeNaN();
      expect(none?.offset).toBeNaN();
      expect(none?.related).toBe(false);
    }

    transport.send({ type: "compare", ids: [4] });
    transport.requestFrame();
    const cancelling = frames.at(-1);
    expect(cancelling?.comparison?.matches[3]).toMatchObject({
      inverted: true,
    });
    expect(cancelling?.comparison?.sumGain).toBeLessThan(-6);
    expect(cancelling?.comparison?.fixedGain).toBeGreaterThan(2);

    transport.send({ type: "previewFix", on: true });
    transport.requestFrame();
    const fixed = frames.at(-1);
    const peak = (values: Float32Array | undefined) =>
      Math.max(...Array.from(values ?? [], Math.abs));
    expect(peak(fixed?.lanes[4])).toBeGreaterThan(peak(cancelling?.lanes[4]));

    transport.send({ type: "compare", ids: [] });
    transport.requestFrame();
    expect(frames.at(-1)?.lanes).toHaveLength(5); // The sum of this track alone.
    expect(frames.at(-1)?.comparison?.sumGain).toBeCloseTo(0);
  });

  test("sweeps in sync with a simulated transport in Beats mode", () => {
    const { transport, frames } = connect(); // now = 1 s: 2 beats into bar 1 at 120 BPM.
    transport.send({ type: "setSetting", name: "timeBase", value: "beats" });
    transport.send({ type: "setSetting", name: "beatSpan", value: "oneBar" });
    transport.requestFrame();
    const frame = frames.at(-1);
    expect(frame?.synced).toBe(true);
    expect(frame?.cursor).toBeCloseTo(0.5);
    expect(frame?.cycleStart).toBe(0);
    expect(frame?.spanQuarterNotes).toBe(4);
    expect(frame?.barQuarterNotes).toBe(4);
  });

  test("synthesises only the visible part at full resolution", () => {
    const { transport, frames } = connect();
    useSeconds(transport);
    transport.send({ type: "setSetting", name: "timeSpan", value: 0.1 });
    transport.send({ type: "view", start: 0.5, end: 0.6 });
    transport.requestFrame();
    const frame = frames.at(-1);
    expect(frame?.viewStart).toBe(0.5);
    expect(frame?.visibleSamples).toBe(480);
    expect(frame?.mode).toBe(FrameMode.raw); // 480 samples fit in 800 columns.
  });

  test("rolls long spans and reports freeze", () => {
    const { transport, frames } = connect();
    useSeconds(transport);
    transport.send({ type: "setSetting", name: "timeSpan", value: 2 });
    transport.send({ type: "freeze", frozen: true });
    transport.requestFrame();
    expect(frames.at(-1)?.freeRunning).toBe(true);
    expect(frames.at(-1)?.frozen).toBe(true);
  });

  test("measures the Mid in Beats mode, as the engine does", () => {
    const { transport, frames } = connect();
    transport.send({
      type: "setSetting",
      name: "triggerSource",
      value: "left",
    });
    useSeconds(transport);
    transport.requestFrame();
    const left = frames.at(-1)?.peak ?? NaN;
    transport.send({ type: "setSetting", name: "timeBase", value: "beats" });
    transport.requestFrame();
    const beats = frames.at(-1)?.peak ?? NaN;
    // L is 0.6 sin; the Mid is half of L plus R: 0.45 sin + 0.125 sin(2x + 0.7).
    let mid = 0;
    for (let step = 0; step < 10000; step++) {
      const phase = (2 * Math.PI * step) / 10000;
      mid = Math.max(
        mid,
        Math.abs(0.45 * Math.sin(phase) + 0.125 * Math.sin(2 * phase + 0.7)),
      );
    }
    expect(left).toBeCloseTo(0.6, 2);
    expect(beats).toBeCloseTo(mid, 2);
  });

  test("keeps a frozen view's trigger point and pans back to the held audio", () => {
    const { transport, frames } = connect(); // now = 1 s.
    useSeconds(transport);
    transport.send({ type: "setSetting", name: "timeSpan", value: 0.1 });
    transport.requestFrame();
    expect(frames.at(-1)?.panReach).toBe(0); // Live: nothing to pan through.

    // Frozen, the trigger point stays where the live window had it: a span back, at 0.9 s (198
    // periods of 220 Hz). The view reaches back from there to the oldest audio held.
    transport.send({ type: "freeze", frozen: true });
    transport.requestFrame();
    const trigger = 0.9;
    const reach = (span: number) =>
      trigger - triggerPosition * span - (1 - frozenHoldSeconds);
    expect(frames.at(-1)?.panReach).toBeCloseTo(reach(0.1), 3);

    // A longer span grows around the trigger point, so the reach shrinks.
    transport.send({ type: "setSetting", name: "timeSpan", value: 0.5 });
    transport.requestFrame();
    expect(frames.at(-1)?.panReach).toBeCloseTo(reach(0.5), 3);

    // A free-running span ends at the right edge of the last live window instead: the trigger point
    // and the rest of that window after it.
    transport.send({ type: "setSetting", name: "timeSpan", value: 2 });
    transport.requestFrame();
    expect(frames.at(-1)?.freeRunning).toBe(true);
    const liveEnd = trigger + (1 - triggerPosition) * 0.1;
    expect(frames.at(-1)?.panReach).toBeCloseTo(
      liveEnd - 2 - (1 - frozenHoldSeconds),
      3,
    );

    // Unfrozen, nothing to pan through again.
    transport.send({ type: "freeze", frozen: false });
    transport.requestFrame();
    expect(frames.at(-1)?.panReach).toBe(0);
  });

  test("measures L on a mono track, whatever the trigger source", () => {
    const { transport, frames } = connect({ inputChannels: 1 });
    useSeconds(transport);
    transport.send({
      type: "setSetting",
      name: "triggerSource",
      value: "side",
    });
    transport.requestFrame();
    expect(frames.at(-1)?.peak).toBeCloseTo(0.6, 2);
  });

  test("shows a mono track's L alone, whatever views are asked for", () => {
    const { transport, messages, frames } = connect({ inputChannels: 1 });
    expect(messages[0]).toMatchObject({ type: "snapshot", inputChannels: 1 });
    useSeconds(transport);
    transport.send({
      type: "setSetting",
      name: "channelViews",
      value: ["mid", "side"],
    });
    transport.requestFrame();
    expect([...(frames.at(-1)?.views.keys() ?? [])]).toEqual([
      ChannelView.left,
    ]);
  });

  test("runs at the sample rate it is given", () => {
    const { transport, messages, frames } = connect({ sampleRate: 96000 });
    expect(messages[0]).toMatchObject({ type: "snapshot", sampleRate: 96000 });
    useSeconds(transport);
    transport.send({ type: "setSetting", name: "timeSpan", value: 0.1 });
    transport.send({ type: "view", start: 0.5, end: 0.6 });
    transport.send({
      type: "setSetting",
      name: "displayMode",
      value: "compare",
    });
    transport.send({ type: "compare", ids: [4] });
    transport.requestFrame();
    const frame = frames.at(-1);
    expect(frame?.visibleSamples).toBe(960);
    expect(frame?.sampleRate).toBe(96000);
    // Kick 2 is 1.3 ms late: 125 samples at 96 kHz.
    expect(frame?.comparison?.matches[3]?.offset).toBe(125);
  });

  test("places a view frozen while running free around that window", () => {
    const { transport, frames } = connect(); // now = 1 s.
    useSeconds(transport);
    transport.send({ type: "setSetting", name: "timeSpan", value: 2 });
    transport.send({ type: "freeze", frozen: true });
    // The live 2 s window ended at 1 s. At 100 ms the trigger point sits one trigger offset into
    // that window, and the view starts one trigger offset of its own span before it.
    transport.send({ type: "setSetting", name: "timeSpan", value: 0.1 });
    transport.requestFrame();
    const anchor = 1 - 2 + triggerPosition * 2;
    expect(frames.at(-1)?.panReach).toBeCloseTo(
      anchor - triggerPosition * 0.1 - (1 - frozenHoldSeconds),
      3,
    );
  });
});

describe("mockSummingGain", () => {
  test.each([
    [2, 1, 10 * Math.log10(2)],
    [1e-40, 1, -400],
    [0, 1, -Infinity], // An exact cancellation, not a number at a floor.
    [0, 1e-300, -Infinity],
  ])("sum %s against parts %s reads %s dB", (sum, parts, expected) => {
    expect(mockSummingGain(sum, parts)).toBeCloseTo(expected);
  });
  test.each([
    [0, 0],
    [1, 0],
  ])("is NaN, not 0, for silent tracks (%s, %s)", (sum, parts) => {
    expect(mockSummingGain(sum, parts)).toBeNaN();
  });
});
