// The replay of a recorded engine session (ui/tests/app/replay-transport.ts): what it hands the
// page, and when it lets the page's messages through.
import { describe, expect, test } from "vitest";

import { FrameMode } from "../../protocol/src/enums.ts";
import { decodeFrame } from "../../protocol/src/frame-decoder.ts";
import { encodeFrame } from "../../protocol/src/frame-encoder.ts";
import type { ControlMessage } from "../src/bridge/transport.ts";
import {
  describeUnexpected,
  loadRecording,
  matches,
  type Recording,
  ReplayTransport,
  replayTolerance,
} from "./app/replay-transport.ts";

// A frame told apart by its peak, showing `spanSeconds`.
function frame(peak: number, spanSeconds = 1): string {
  const bytes = new Uint8Array(
    encodeFrame({
      mode: FrameMode.raw,
      triggered: false,
      frozen: false,
      roll: false,
      pointCount: 0,
      startFraction: 0,
      peak,
      rms: 0,
      frequency: undefined,
      sampleRate: 48000,
      spanSeconds,
      views: new Map(),
    }),
  );
  return btoa(String.fromCharCode(...bytes));
}

const viewStart = 0.25;
const viewEnd = 0.75;
const panSeconds = 0.5;
const view: ControlMessage = { type: "view", start: viewStart, end: viewEnd };
const pan: ControlMessage = { type: "pan", seconds: panSeconds };

const recording: Recording = {
  scenario: "Test",
  protocolVersion: 0,
  snapshot: { type: "snapshot", sampleRate: 48000 },
  phases: [
    {
      after: null,
      answers: [],
      replies: [{ type: "lanes", id: 1 }],
      frames: [frame(1, 2)],
    },
    {
      after: view,
      answers: [{ type: "stateChanged", name: "x" }],
      replies: [],
      frames: [frame(2, 2)],
    },
    { after: pan, answers: [], replies: [], frames: [frame(3, 2)] },
    {
      after: { host: "sampleRate", rate: 44100 },
      answers: [],
      replies: [{ type: "snapshot", sampleRate: 44100 }],
      frames: [],
    },
  ],
};

// A replay that runs its work when the test says, and what it handed the page.
function started(of: Recording = recording) {
  const work: (() => void)[] = [];
  const replay = new ReplayTransport(of, (task) => work.push(task));
  const messages: ControlMessage[] = [];
  const peaks: number[] = [];
  replay.onMessage((message) => messages.push(message));
  replay.onFrame((bytes) => peaks.push(decodeFrame(bytes).peak));
  const run = () => {
    while (work.length > 0) {
      work.shift()?.();
    }
  };
  return { replay, messages, peaks, run };
}

describe("a replay of a recorded session", () => {
  test("answers hello with the recorded snapshot, then its replies after the first frame", () => {
    const { replay, messages, peaks, run } = started();
    replay.send({ type: "hello" });
    run();
    expect(messages.map((m) => m.type)).toEqual(["snapshot"]);
    replay.requestFrame();
    run();
    expect(peaks).toEqual([1]);
    expect(messages.map((m) => m.type)).toEqual(["snapshot", "lanes"]);
  });

  test("repeats the phase's frame for every request", () => {
    const { replay, peaks, run } = started();
    replay.send({ type: "hello" });
    for (let i = 0; i < 3; i++) {
      replay.requestFrame();
    }
    run();
    expect(peaks).toEqual([1, 1, 1]);
  });

  test("moves to the next phase on its message, and answers it at once", () => {
    const { replay, messages, peaks, run } = started();
    replay.send({ type: "hello" });
    replay.send({ ...view });
    run();
    expect(replay.currentPhase).toBe(1);
    expect(messages.map((m) => m.type)).toEqual(["snapshot", "stateChanged"]);
    replay.requestFrame();
    run();
    expect(peaks).toEqual([2]);
  });

  test("takes a view within the tolerance of its width, but not one just outside it", () => {
    const width = viewEnd - viewStart;
    const within = {
      ...view,
      start: viewStart + 0.5 * replayTolerance * width,
    };
    const outside = {
      ...view,
      start: viewStart + 2 * replayTolerance * width,
    };
    expect(matches(within, view, 1)).toBe(true);
    expect(matches(outside, view, 1)).toBe(false);
  });

  test("takes a pan within the tolerance of the span, but not one just outside it", () => {
    const span = 2;
    const within = {
      ...pan,
      seconds: panSeconds + 0.5 * replayTolerance * span,
    };
    const outside = {
      ...pan,
      seconds: panSeconds + 2 * replayTolerance * span,
    };
    expect(matches(within, pan, span)).toBe(true);
    expect(matches(outside, pan, span)).toBe(false);
  });

  test("keeps the phase on a view that is a step on the way to the recorded one", () => {
    const { replay, peaks, run } = started();
    replay.send({ type: "hello" });
    replay.send({ type: "view", start: 0.1, end: 0.9 });
    replay.requestFrame();
    run();
    expect(replay.currentPhase).toBe(0);
    expect(replay.steps).toHaveLength(1);
    expect(replay.unexpected).toEqual([]);
    expect(peaks).toEqual([1]);
  });

  test("collects a message the recording does not hold, with what it expected", () => {
    const { replay } = started();
    replay.send({ type: "hello" });
    const setting = { type: "setSetting", name: "timeSpan", value: 2 } as const;
    replay.send(setting);
    expect(replay.unexpected).toEqual([{ message: setting, expected: view }]);
    expect(describeUnexpected("Test", replay.unexpected)).toMatch(
      /setSetting.*does not hold; it expected .*"view"/u,
    );
    expect(replay.currentPhase).toBe(0);
  });

  test("ignores the page's viewport, which the recorded columns do not follow", () => {
    const { replay } = started();
    replay.send({ type: "hello" });
    replay.send({ type: "viewport", viewWidth: 500, pixelRatio: 2 });
    expect(replay.unexpected).toEqual([]);
    expect(replay.currentPhase).toBe(0);
  });

  test("enters a host phase only when the test says, and keeps the frame before it", () => {
    const { replay, messages, peaks, run } = started();
    replay.send({ type: "hello" });
    replay.send({ ...view });
    replay.send({ ...pan });
    expect(replay.currentPhase).toBe(2);
    replay.hostEvent();
    run();
    expect(replay.currentPhase).toBe(3);
    expect(messages.at(-1)).toEqual({ type: "snapshot", sampleRate: 44100 });
    replay.requestFrame();
    run();
    expect(peaks).toEqual([3]);
  });

  test("refuses a host event the recording does not hold next", () => {
    const { replay } = started();
    replay.send({ type: "hello" });
    expect(() => {
      replay.hostEvent();
    }).toThrow(/not a host event/u);
  });

  test("says how to write a recording that is missing", async () => {
    await expect(loadRecording("NoSuchScenario", {})).rejects.toThrow(
      /NoSuchScenario\.json is missing.*ORIZON_UPDATE_RECORDINGS=1/u,
    );
  });
});
