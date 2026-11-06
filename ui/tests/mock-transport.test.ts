import { describe, expect, test } from "vitest";

import {
  ChannelView,
  DisplayMode,
  FrameMode,
  TimeBase,
  Toggle,
} from "../../protocol/src/enums.ts";
import { decodeFrame, type Frame } from "../../protocol/src/frame-decoder.ts";
import {
  frozenHoldSeconds,
  triggerPosition,
} from "../../protocol/src/limits.ts";
import {
  mockFundamental,
  mockSummingGain,
  MockTransport,
  viewSample,
  type MockOptions,
} from "../src/bridge/mock-transport.ts";
import type { ControlMessage } from "../src/bridge/transport.ts";
import { viewOrder } from "../src/view/screen-state.ts";

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

// Beats is the default time base; these tests use Trigger, or Free.
function useTrigger(transport: MockTransport): void {
  transport.send({ type: "setSetting", name: "timeBase", value: "trigger" });
}

// The Free time base at `span` seconds.
function runFree(transport: MockTransport, span: number): void {
  transport.send({ type: "setSetting", name: "timeBase", value: "free" });
  transport.send({ type: "setSetting", name: "freeSpan", value: span });
}

describe("mock back end", () => {
  // As the editor does: what it says of itself first, then the engine's snapshot.
  test("answers hello with about and a snapshot, and the first request with a frame", () => {
    const { messages, frames } = connect();
    expect(messages[0]).toMatchObject({ type: "about", contact: true });
    expect(messages[1]).toMatchObject({
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
    useTrigger(transport);
    transport.send({
      type: "setSetting",
      name: "channelViews",
      value: ["mid", "side"],
    });
    transport.send({ type: "setSetting", name: "triggerSpan", value: 0.1 });
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
    transport.send({ type: "setSetting", name: "triggerSpan", value: 99 });
    expect(
      messages.filter((message) => message.type === "stateChanged"),
    ).toEqual([
      { type: "stateChanged", name: "triggerMode", value: "normal" },
      { type: "stateChanged", name: "triggerSpan", value: 1 },
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
    transport.send({ type: "setSetting", name: "displayMode", value: "multi" });
    transport.send({ type: "setSetting", name: "compare", value: "on" });
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

  // As the engine decides: Compare is on show only with the switch on in Multi. The app tests run
  // against this mock, so a wrong predicate here would let them pass on a wrong page.
  test.each([
    ["single", "off", false],
    ["single", "on", false],
    ["multi", "off", false],
    ["multi", "on", true],
  ] as const)(
    "sends Compare frames in %s with Compare %s: %s",
    (displayMode, compare, comparing) => {
      const { transport, frames } = connect();
      transport.send({
        type: "setSetting",
        name: "displayMode",
        value: displayMode,
      });
      transport.send({ type: "setSetting", name: "compare", value: compare });
      transport.requestFrame();
      expect(frames.at(-1)?.comparison !== undefined).toBe(comparing);
    },
  );

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
    useTrigger(transport);
    transport.send({ type: "setSetting", name: "triggerSpan", value: 0.1 });
    transport.send({ type: "view", start: 0.5, end: 0.6 });
    transport.requestFrame();
    const frame = frames.at(-1);
    expect(frame?.viewStart).toBe(0.5);
    expect(frame?.visibleSamples).toBe(480);
    expect(frame?.mode).toBe(FrameMode.raw); // 480 samples fit in 800 columns.
  });

  test("runs the Free time base free and reports freeze", () => {
    const { transport, frames } = connect();
    runFree(transport, 2);
    transport.send({ type: "freeze", frozen: true });
    transport.requestFrame();
    expect(frames.at(-1)?.freeRunning).toBe(true);
    expect(frames.at(-1)?.frozen).toBe(true);
  });

  // As the engine does: the readout source when drawn, else the first view drawn, whatever the
  // trigger reads, and the frame names it. Every time base, every source, with every set of views.
  test.each(
    Object.keys(TimeBase).flatMap((timeBase) =>
      viewOrder.map((source) => [timeBase, source] as const),
    ),
  )("measures the readout source in %s when drawn: %s", (timeBase, source) => {
    for (let drawn = 1; drawn < 1 << viewOrder.length; drawn++) {
      const { transport, frames } = connect();
      const views = viewOrder.filter(
        (_, index) => (drawn & (1 << index)) !== 0,
      );
      transport.send({
        type: "setSetting",
        name: "triggerSource",
        value: "mid",
      });
      transport.send({
        type: "setSetting",
        name: "readoutSource",
        value: source,
      });
      transport.send({
        type: "setSetting",
        name: "channelViews",
        value: views,
      });
      useTrigger(transport);
      transport.send({ type: "setSetting", name: "timeBase", value: timeBase });
      transport.requestFrame();
      const measured =
        ChannelView[views.includes(source) ? source : (views[0] ?? "left")];
      let peak = 0;
      for (let step = 0; step < 10000; step++) {
        peak = Math.max(
          peak,
          Math.abs(viewSample(measured, step / 10000 / mockFundamental)),
        );
      }
      expect(frames.at(-1)?.readoutView, views.join(" ")).toBe(measured);
      expect(frames.at(-1)?.peak, views.join(" ")).toBeCloseTo(peak, 2);
    }
  });

  // As the engine does: the trigger source when drawn, else the first view drawn, a mono track's L
  // alone, and this track's Mid in Multi, Compare off or on; the frame names it. Every source,
  // every set of views, every display mode with Compare off and on.
  test.each(
    Object.keys(DisplayMode).flatMap((mode) =>
      Object.keys(Toggle).flatMap((compare) =>
        viewOrder.map((source) => [mode, compare, source] as const),
      ),
    ),
  )(
    "names the view the trigger reads in %s with Compare %s: %s",
    (mode, compare, source) => {
      for (const inputChannels of [2, 1] as const) {
        for (let drawn = 1; drawn < 1 << viewOrder.length; drawn++) {
          const { transport, frames } = connect({ inputChannels });
          const views = viewOrder.filter(
            (_, index) => (drawn & (1 << index)) !== 0,
          );
          transport.send({
            type: "setSetting",
            name: "triggerSource",
            value: source,
          });
          transport.send({
            type: "setSetting",
            name: "channelViews",
            value: views,
          });
          transport.send({
            type: "setSetting",
            name: "displayMode",
            value: mode,
          });
          transport.send({
            type: "setSetting",
            name: "compare",
            value: compare,
          });
          useTrigger(transport);
          transport.requestFrame();
          const shown: readonly (typeof viewOrder)[number][] =
            inputChannels === 1 ? ["left"] : views;
          const expected =
            mode === "single"
              ? ChannelView[
                  shown.includes(source) ? source : (shown[0] ?? "left")
                ]
              : ChannelView.mid;
          expect(
            frames.at(-1)?.triggerView,
            `${views.join(" ")}, ${String(inputChannels)}`,
          ).toBe(expected);
        }
      }
    },
  );

  // A frame with no tempo yet names the channel too, and Multi, Compare off or on, by its lane set.
  test.each(
    Object.keys(DisplayMode).flatMap((mode) =>
      Object.keys(Toggle).map((compare) => [mode, compare] as const),
    ),
  )(
    "names the readout channel with no tempo in %s with Compare %s",
    (mode, compare) => {
      const { transport, frames } = connect({ hostTempo: false });
      transport.send({
        type: "setSetting",
        name: "readoutSource",
        value: "side",
      });
      transport.send({
        type: "setSetting",
        name: "channelViews",
        value: ["mid", "side"],
      });
      transport.send({ type: "setSetting", name: "displayMode", value: mode });
      transport.send({ type: "setSetting", name: "compare", value: compare });
      transport.send({ type: "setSetting", name: "timeBase", value: "beats" });
      transport.requestFrame();
      const frame = frames.at(-1);
      expect(frame?.noTempo).toBe(true);
      expect(frame?.readoutView).toBe(
        mode === "single" ? ChannelView.side : ChannelView.mid,
      );
      expect(frame?.laneSet !== 0).toBe(mode !== "single");
    },
  );

  test("keeps a frozen view's trigger point and pans back to the held audio", () => {
    const { transport, frames } = connect(); // now = 1 s.
    useTrigger(transport);
    transport.send({ type: "setSetting", name: "triggerSpan", value: 0.1 });
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
    transport.send({ type: "setSetting", name: "triggerSpan", value: 0.5 });
    transport.requestFrame();
    expect(frames.at(-1)?.panReach).toBeCloseTo(reach(0.5), 3);

    // Free ends at the right edge of the last live window instead: the trigger point and the rest
    // of that window after it.
    runFree(transport, 2);
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

  test("measures L on a mono track, whatever the trigger and readout sources", () => {
    const { transport, frames } = connect({ inputChannels: 1 });
    useTrigger(transport);
    transport.send({
      type: "setSetting",
      name: "triggerSource",
      value: "side",
    });
    transport.send({
      type: "setSetting",
      name: "readoutSource",
      value: "side",
    });
    transport.requestFrame();
    expect(frames.at(-1)?.readoutView).toBe(ChannelView.left);
    expect(frames.at(-1)?.peak).toBeCloseTo(0.6, 2);
  });

  test("shows a mono track's L alone, whatever views are asked for", () => {
    const { transport, messages, frames } = connect({ inputChannels: 1 });
    expect(messages[1]).toMatchObject({ type: "snapshot", inputChannels: 1 });
    useTrigger(transport);
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
    expect(messages[1]).toMatchObject({ type: "snapshot", sampleRate: 96000 });
    useTrigger(transport);
    transport.send({ type: "setSetting", name: "triggerSpan", value: 0.1 });
    transport.send({ type: "view", start: 0.5, end: 0.6 });
    transport.send({ type: "setSetting", name: "displayMode", value: "multi" });
    transport.send({ type: "setSetting", name: "compare", value: "on" });
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
    runFree(transport, 2);
    transport.send({ type: "freeze", frozen: true });
    // The live 2 s window ended at 1 s. In Trigger at 100 ms the view is the 100 ms ending there,
    // as the engine shows it, and reaches back to the oldest audio held.
    useTrigger(transport);
    transport.send({ type: "setSetting", name: "triggerSpan", value: 0.1 });
    transport.requestFrame();
    expect(frames.at(-1)?.freeRunning).toBe(false);
    expect(frames.at(-1)?.panReach).toBeCloseTo(
      1 - 0.1 - (1 - frozenHoldSeconds),
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
