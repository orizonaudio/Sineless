// A back end that runs without the plugin (DESIGN 5.1): it answers the session protocol like
// the engine and synthesises frames in the real binary format, so the UI can be developed in a
// browser through the Vite dev server.
import {
  BeatSpan,
  beatSpanQuarterNotes,
  ChannelView,
  FrameMode,
} from "../../../protocol/src/enums.ts";
import { encodeFrame } from "../../../protocol/src/frame-encoder.ts";
import type { SettingValue } from "../../../protocol/src/messages.ts";
import {
  defaultSettingValues,
  isSettingName,
  isValidSettingValue,
} from "../../../protocol/src/setting-values.ts";
import type { SettingName } from "../../../protocol/src/settings.ts";
import { protocolVersion } from "../../../protocol/src/version.ts";
import type { ControlMessage, Transport } from "./transport.ts";

export const mockSampleRate = 48000;
// The synthetic signal: L is a 220 Hz tone, R adds its octave, so every view repeats every
// 220 Hz period and the display stays as steady as a triggered one.
export const mockFundamental = 220;
// The simulated transport (FR-9): 120 BPM in 4/4.
export const mockTempo = 120;
const mockBarQuarterNotes = 4;
const triggerPosition = 0.1;
const rollThresholdSeconds = 1;

function signal(channel: "left" | "right", time: number): number {
  const phase = 2 * Math.PI * mockFundamental * time;
  return channel === "left"
    ? 0.6 * Math.sin(phase)
    : 0.3 * Math.sin(phase) + 0.25 * Math.sin(2 * phase + 0.7);
}

function viewSample(view: ChannelView, time: number): number {
  const left = signal("left", time);
  const right = signal("right", time);
  switch (view) {
    case ChannelView.left:
      return left;
    case ChannelView.right:
      return right;
    case ChannelView.mid:
      return 0.5 * (left + right);
    case ChannelView.side:
      return 0.5 * (left - right);
  }
}

export interface MockOptions {
  // Seconds since start; real time by default.
  readonly now?: () => number;
  // Runs work asynchronously, like a message from another process.
  readonly schedule?: (work: () => void) => void;
}

export class MockTransport implements Transport {
  private messageHandler: ((message: ControlMessage) => void) | undefined;
  private frameHandler: ((frame: ArrayBuffer) => void) | undefined;
  private readonly settings = defaultSettingValues();
  private readonly now: () => number;
  private readonly schedule: (work: () => void) => void;
  private columns = 0;
  private frozenAt: number | undefined;
  // The part of the span on screen (FR-10); only that part is synthesised.
  private view = { start: 0, end: 1 };
  private sequence = 0;

  constructor(options: MockOptions = {}) {
    const start = performance.now();
    this.now = options.now ?? (() => (performance.now() - start) / 1000);
    this.schedule = options.schedule ?? ((work) => setTimeout(work, 0));
  }

  onMessage(handler: (message: ControlMessage) => void): void {
    this.messageHandler = handler;
  }

  onFrame(handler: (frame: ArrayBuffer) => void): void {
    this.frameHandler = handler;
  }

  send(message: ControlMessage): void {
    this.schedule(() => {
      this.receive(message);
    });
  }

  private reply(message: ControlMessage): void {
    this.messageHandler?.(message);
  }

  private receive(message: ControlMessage): void {
    switch (message.type) {
      case "hello":
        this.setViewport(message);
        this.reply({
          type: "snapshot",
          protocolVersion,
          settings: { ...this.settings },
          sampleRate: mockSampleRate,
          inputChannels: 2,
        });
        this.sendFrame(); // The first frame needs no credit (AD-12).
        break;
      case "setSetting":
        this.setSetting(message["name"], message["value"]);
        break;
      case "viewport":
        this.setViewport(message);
        break;
      case "freeze":
        this.frozenAt =
          message["frozen"] === true
            ? (this.frozenAt ?? this.now())
            : undefined;
        break;
      case "view": {
        const start = message["start"];
        const end = message["end"];
        if (
          typeof start === "number" &&
          typeof end === "number" &&
          start >= 0 &&
          end <= 1 &&
          end > start
        ) {
          this.view = { start, end };
        }
        break;
      }
      case "credit":
        this.sendFrame();
        break;
      default:
        break;
    }
  }

  private setSetting(name: unknown, value: unknown): void {
    if (!isSettingName(name)) {
      return;
    }
    if (isValidSettingValue(name, value)) {
      this.settings[name] = value;
    }
    this.reply({ type: "stateChanged", name, value: this.settings[name] });
  }

  private setViewport(message: ControlMessage): void {
    const width = message["viewWidth"];
    const ratio = message["pixelRatio"];
    if (
      typeof width === "number" &&
      typeof ratio === "number" &&
      width > 0 &&
      ratio > 0
    ) {
      this.columns = Math.max(1, Math.round(width * ratio));
    }
  }

  private setting(name: SettingName): SettingValue {
    return this.settings[name];
  }

  private sendFrame(): void {
    if (this.columns === 0) {
      return;
    }
    const time = this.frozenAt ?? this.now();
    const synced = this.setting("timeBase") === "beats";
    let span = this.setting("timeSpan") as number;
    let timeOf: (fraction: number) => number;
    let cursor: number | undefined;
    let cycleStart = 0;
    let spanQuarterNotes = 0;
    const roll = !synced && span > rollThresholdSeconds;
    if (synced) {
      // A transport playing at the mock tempo since start-up (FR-9).
      const name = this.setting("beatSpan") as keyof typeof BeatSpan;
      spanQuarterNotes = beatSpanQuarterNotes(
        BeatSpan[name],
        mockBarQuarterNotes,
      );
      const secondsPerQuarterNote = 60 / mockTempo;
      span = spanQuarterNotes * secondsPerQuarterNote;
      const playhead = time / secondsPerQuarterNote;
      cycleStart = Math.floor(playhead / spanQuarterNotes) * spanQuarterNotes;
      const phase = (playhead - cycleStart) / spanQuarterNotes;
      const cycleTime = cycleStart * secondsPerQuarterNote;
      cursor = phase;
      timeOf = (fraction) =>
        (fraction < phase ? cycleTime : cycleTime - span) + fraction * span;
    } else {
      // Triggered spans start one trigger offset before a period boundary; roll spans end now.
      const period = 1 / mockFundamental;
      const start = roll
        ? time - span
        : Math.floor((time - span) / period) * period - triggerPosition * span;
      timeOf = (fraction) => start + fraction * span;
    }

    const visibleWidth = this.view.end - this.view.start;
    const sampleCount = Math.max(
      1,
      Math.round(span * visibleWidth * mockSampleRate),
    );
    const sampleTime = (sample: number) =>
      timeOf(this.view.start + (sample / sampleCount) * visibleWidth);
    const minMax = sampleCount > this.columns;
    const pointCount = minMax ? this.columns : sampleCount;
    const samplesPerPoint = sampleCount / pointCount;
    const names = this.setting("channelViews") as readonly string[];
    const views = new Map<ChannelView, Float32Array>();
    for (const [name, view] of Object.entries(ChannelView)) {
      if (!names.includes(name)) {
        continue;
      }
      const points = new Float32Array(minMax ? pointCount * 2 : pointCount);
      for (let point = 0; point < pointCount; point++) {
        if (!minMax) {
          points[point] = viewSample(view, sampleTime(point));
          continue;
        }
        let low = Infinity;
        let high = -Infinity;
        const first = Math.floor(point * samplesPerPoint);
        const last = Math.floor((point + 1) * samplesPerPoint);
        // Long spans: sample each column sparsely, enough for a mock.
        const stride = Math.max(1, Math.floor((last - first) / 64));
        for (let sample = first; sample < last; sample += stride) {
          const value = viewSample(view, sampleTime(sample));
          low = Math.min(low, value);
          high = Math.max(high, value);
        }
        points[point * 2] = low;
        points[point * 2 + 1] = high;
      }
      views.set(view, points);
    }

    const source =
      ChannelView[this.setting("triggerSource") as keyof typeof ChannelView];
    let sumOfSquares = 0;
    let peak = 0;
    const readoutStride = Math.max(1, Math.floor(sampleCount / 4096));
    let readoutSamples = 0;
    for (let sample = 0; sample < sampleCount; sample += readoutStride) {
      const value = viewSample(source, sampleTime(sample));
      sumOfSquares += value * value;
      peak = Math.max(peak, Math.abs(value));
      readoutSamples++;
    }

    this.frameHandler?.(
      encodeFrame({
        sequence: this.sequence++,
        mode: minMax ? FrameMode.minMax : FrameMode.raw,
        triggered: !roll && !synced,
        frozen: this.frozenAt !== undefined,
        roll,
        synced,
        cursor,
        cycleStart,
        spanQuarterNotes,
        barQuarterNotes: synced ? mockBarQuarterNotes : 0,
        viewStart: this.view.start,
        viewEnd: this.view.end,
        visibleSamples: sampleCount,
        pointCount,
        samplesPerPoint: minMax ? samplesPerPoint : 1,
        startFraction: 0,
        peak,
        rms: Math.sqrt(sumOfSquares / Math.max(readoutSamples, 1)),
        frequency: mockFundamental,
        views,
      }),
    );
  }
}
