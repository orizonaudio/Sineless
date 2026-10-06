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
    expect(messages.slice(1)).toEqual([
      { type: "stateChanged", name: "triggerMode", value: "normal" },
      { type: "stateChanged", name: "timeSpan", value: 0.02 },
    ]);
  });

  test("rolls long spans and reports freeze", () => {
    const { transport, frames } = connect();
    transport.send({ type: "setSetting", name: "timeSpan", value: 2 });
    transport.send({ type: "freeze", frozen: true });
    transport.send({ type: "credit" });
    expect(frames.at(-1)?.roll).toBe(true);
    expect(frames.at(-1)?.frozen).toBe(true);
  });
});
