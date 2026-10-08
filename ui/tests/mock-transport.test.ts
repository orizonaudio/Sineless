import { describe, expect, test } from "vitest";

import { ChannelView, FrameMode } from "../../protocol/src/enums.ts";
import { decodeFrame, type Frame } from "../../protocol/src/frame-decoder.ts";
import {
  mockFundamental,
  MockTransport,
} from "../src/bridge/mock-transport.ts";
import type { ControlMessage } from "../src/bridge/transport.ts";

function connect(): {
  transport: MockTransport;
  messages: ControlMessage[];
  frames: Frame[];
} {
  const transport = new MockTransport({
    now: () => 1.0,
    schedule: (work) => {
      work();
    },
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
  return { transport, messages, frames };
}

// Beats is the default time base; these tests use seconds.
function useSeconds(transport: MockTransport): void {
  transport.send({ type: "setSetting", name: "timeBase", value: "seconds" });
}

describe("mock back end", () => {
  test("answers hello with a snapshot and a first frame", () => {
    const { messages, frames } = connect();
    expect(messages[0]).toMatchObject({
      type: "snapshot",
      sampleRate: 48000,
      inputChannels: 2,
    });
    expect(frames).toHaveLength(1);
  });

  test("sends one frame per credit and none without", () => {
    const { transport, frames } = connect();
    transport.send({ type: "credit" });
    transport.send({ type: "credit" });
    expect(frames).toHaveLength(3);
    expect(frames.map((frame) => frame.sequence)).toEqual([0, 1, 2]);
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
    transport.send({ type: "credit" });
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
    transport.send({ type: "credit" });
    const frame = frames.at(-1);
    expect(frame?.views.size).toBe(0);
    expect(frame?.lanes).toHaveLength(4);
    expect(frame?.laneSet).toBe(1);

    transport.send({ type: "setSetting", name: "group", value: "b" });
    transport.send({ type: "credit" });
    expect(lanes()).toHaveLength(2);
    expect(lanes()[1]).toMatchObject({
      laneSet: 2,
      lanes: [{ name: "Kick", own: true }],
    });
    expect(frames.at(-1)?.lanes).toHaveLength(1);
  });

  test("compares this track with a partner and previews the fix", () => {
    const { transport, messages, frames } = connect();
    transport.send({
      type: "setSetting",
      name: "displayMode",
      value: "compare",
    });
    transport.send({ type: "credit" });
    const lanes = messages.filter((message) => message.type === "lanes").at(-1);
    expect(lanes).toMatchObject({
      lanes: [{ name: "Kick" }, { name: "Bass" }],
    });
    expect(lanes?.["candidates"]).toHaveLength(3);
    expect(frames.at(-1)?.lanes).toHaveLength(3);
    expect(frames.at(-1)?.comparison?.sumGain).toBeCloseTo(0.1);

    transport.send({ type: "partner", id: 4 });
    transport.send({ type: "credit" });
    const cancelling = frames.at(-1);
    expect(cancelling?.comparison).toMatchObject({ inverted: true });
    expect(cancelling?.comparison?.sumGain).toBeLessThan(-6);

    transport.send({ type: "previewFix", on: true });
    transport.send({ type: "credit" });
    const fixed = frames.at(-1);
    const peak = (values: Float32Array | undefined) =>
      Math.max(...Array.from(values ?? [], Math.abs));
    expect(peak(fixed?.lanes[2])).toBeGreaterThan(peak(cancelling?.lanes[2]));
  });

  test("sweeps in sync with a simulated transport in Beats mode", () => {
    const { transport, frames } = connect(); // now = 1 s: 2 beats into bar 1 at 120 BPM.
    transport.send({ type: "setSetting", name: "timeBase", value: "beats" });
    transport.send({ type: "setSetting", name: "beatSpan", value: "oneBar" });
    transport.send({ type: "credit" });
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
    transport.send({ type: "credit" });
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
    transport.send({ type: "credit" });
    expect(frames.at(-1)?.roll).toBe(true);
    expect(frames.at(-1)?.frozen).toBe(true);
  });
});
