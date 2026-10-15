// A back end that runs without the plugin: it answers the session protocol like
// the engine and synthesises frames in the real binary format, so the UI can be developed in a
// browser through the Vite dev server.
import {
  BeatSpan,
  beatSpanQuarterNotes,
  ChannelView,
  FrameMode,
} from "../../../protocol/src/enums.ts";
import type { Match } from "../../../protocol/src/frame-decoder.ts";
import {
  encodeFrame,
  type FrameContent,
} from "../../../protocol/src/frame-encoder.ts";
import type {
  LaneLabel,
  SettingValue,
} from "../../../protocol/src/messages.ts";
import {
  defaultSettingValues,
  isSettingName,
  isValidSettingValue,
} from "../../../protocol/src/setting-values.ts";
import type { SettingName } from "../../../protocol/src/settings.ts";
import {
  frozenHoldSeconds,
  rollThresholdSeconds,
  triggerPosition,
} from "../../../protocol/src/limits.ts";
import { protocolVersion } from "../../../protocol/src/version.ts";
import { clampPan, readoutView, type ViewName } from "../view/screen-state.ts";
import type { ControlMessage, Transport } from "./transport.ts";

export const mockSampleRate = 48000;
// The synthetic signal: L is a 220 Hz tone, R adds its octave, so every view repeats every
// 220 Hz period and the display stays as steady as a triggered one.
export const mockFundamental = 220;
// The simulated transport: 120 BPM in 4/4.
export const mockTempo = 120;
export const mockBarQuarterNotes = 4;

function signal(channel: "left" | "right", time: number): number {
  const phase = 2 * Math.PI * mockFundamental * time;
  return channel === "left"
    ? 0.6 * Math.sin(phase)
    : 0.3 * Math.sin(phase) + 0.25 * Math.sin(2 * phase + 0.7);
}

// On a mono track R repeats L, as a host feeds a mono track to a stereo insert.
function viewSample(view: ChannelView, time: number, mono = false): number {
  const left = signal("left", time);
  const right = mono ? left : signal("right", time);
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

// Tracks of the simulated multi-track view, all in group A: this instance's kick on every beat,
// a bass line, unnamed hi-hats on the eighths without a host colour, and a second kick mic that
// is inverted and 1.3 ms late. Each carries what the Compare view would find against the kick,
// and the signal as the fix would correct it.
const beatSeconds = 60 / 120;
function decaying(time: number, every: number, rate: number): number {
  const since = ((time % every) + every) % every;
  return Math.exp(-since * rate);
}
function kick(time: number): number {
  return (
    0.9 *
    decaying(time, beatSeconds, 9) *
    Math.sin(
      2 * Math.PI * 52 * (((time % beatSeconds) + beatSeconds) % beatSeconds),
    )
  );
}
const kickMicDelay = 0.0013;
const unrelated: Match = {
  correlation: 0.01,
  related: false,
  offset: Number.NaN,
  inverted: false,
};
// This track's, and a track's not compared: none, as the engine sends.
const noMatch: Match = {
  correlation: Number.NaN,
  related: false,
  offset: Number.NaN,
  inverted: false,
};
export const mockTracks: readonly (Omit<LaneLabel, "compared"> & {
  readonly sample: (time: number) => number;
  readonly fixed?: (time: number) => number;
  // What the Compare view finds against this track; its offset in samples follows from `delay`.
  readonly match?: Omit<Match, "offset">;
  readonly delay?: number; // Seconds.
})[] = [
  {
    id: 1,
    name: "Kick",
    colour: 0xe0533d,
    own: true,
    sample: kick,
  },
  {
    id: 2,
    name: "Bass",
    colour: 0x4f8fe6,
    own: false,
    sample: (time) =>
      0.45 *
      (1 - 0.8 * decaying(time, beatSeconds, 6)) *
      Math.sin(2 * Math.PI * 55 * time),
  },
  {
    id: 3,
    name: "",
    colour: null,
    own: false,
    sample: (time) =>
      0.35 *
      decaying(time, beatSeconds / 2, 40) *
      Math.sin(2 * Math.PI * 6100 * time) *
      Math.sin(2 * Math.PI * 9300 * time),
  },
  {
    id: 4,
    name: "Kick 2",
    colour: 0xe8a35a,
    own: false,
    sample: (time) => -0.8 * kick(time - kickMicDelay),
    fixed: (time) => 0.8 * kick(time),
    match: { correlation: -0.93, related: true, inverted: true },
    delay: kickMicDelay,
  },
];

// A summing gain in dB from the sum's energy and the tracks' energies added, raw as the engine's
// summingGain sends it: -Infinity when the sum cancels exactly, NaN when the tracks are silent.
// The gain floor is the formatter's.
export function mockSummingGain(
  sumEnergy: number,
  partsEnergy: number,
): number {
  return partsEnergy > 0
    ? 10 * Math.log10(sumEnergy / partsEnergy)
    : Number.NaN;
}

export interface MockOptions {
  // Seconds since start; real time by default.
  readonly now?: () => number;
  // Runs work asynchronously, like a message from another process.
  readonly schedule?: (work: () => void) => void;
  // The host's sample rate; mockSampleRate by default.
  readonly sampleRate?: number;
  // The track's channels: 1 for a mono track, whose R repeats L and which shows L alone.
  readonly inputChannels?: 1 | 2;
  // Samples of this track that are not finite (NaN or infinite) in what each frame measures, as
  // from a broken plugin upstream; none by default.
  readonly brokenSamples?: number;
}

export class MockTransport implements Transport {
  private messageHandler: ((message: ControlMessage) => void) | undefined;
  private frameHandler: ((frame: ArrayBuffer) => void) | undefined;
  private readonly settings = defaultSettingValues();
  private readonly now: () => number;
  private readonly schedule: (work: () => void) => void;
  private columns = 0;
  private frozenAt: number | undefined;
  // The part of the span on screen; only that part is synthesised.
  private view = { start: 0, end: 1 };
  private laneSet = 0;
  private lanesKey = "";
  // The Compare view's other tracks, by id (until asked, the first), and whether the sum previews
  // the fix. The checks outlive the page, as the engine keeps them with the instance.
  private compared: readonly number[] | undefined;
  private previewFix = false;
  private readonly sampleRate: number;
  private readonly inputChannels: 1 | 2;
  private readonly brokenSamples: number;
  // How far back the frozen view is panned, within its reach (see frame panReach).
  private panSeconds = 0;
  // The last live Seconds window before a freeze (none when frozen in Beats mode), from which a
  // frozen view is placed as the engine places it (see frozenWindow).
  private lastLive:
    { readonly start: number; readonly span: number } | undefined;

  constructor(options: MockOptions = {}) {
    const start = performance.now();
    this.now = options.now ?? (() => (performance.now() - start) / 1000);
    this.schedule = options.schedule ?? ((work) => setTimeout(work, 0));
    this.sampleRate = options.sampleRate ?? mockSampleRate;
    this.inputChannels = options.inputChannels ?? 2;
    this.brokenSamples = options.brokenSamples ?? 0;
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

  requestFrame(): void {
    this.schedule(() => {
      this.sendFrame();
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
          sampleRate: this.sampleRate,
          inputChannels: this.inputChannels,
        });
        this.lanesKey = "";
        this.sendLanes();
        break;
      case "setSetting":
        this.setSetting(message["name"], message["value"]);
        break;
      case "viewport":
        this.setViewport(message);
        break;
      case "freeze":
        if (message["frozen"] !== true) {
          this.frozenAt = undefined;
          this.lastLive = undefined;
        } else if (this.frozenAt === undefined) {
          this.frozenAt = this.now();
          const span = this.setting("timeSpan") as number;
          this.lastLive =
            this.setting("timeBase") === "beats"
              ? undefined
              : { start: this.liveStart(this.frozenAt, span), span };
        }
        this.panSeconds = 0;
        break;
      case "pan": {
        const seconds = message["seconds"];
        if (typeof seconds === "number" && Number.isFinite(seconds)) {
          this.panSeconds = clampPan(seconds, this.panReach());
        }
        break;
      }
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
      case "compare": {
        const ids = message["ids"];
        if (Array.isArray(ids)) {
          this.compared = ids.filter((id) => typeof id === "number");
          this.sendLanes();
        }
        break;
      }
      case "previewFix":
        this.previewFix = message["on"] === true;
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
    this.sendLanes();
  }

  // The simulated group: every track in group A, only this instance's track in the others.
  // What the engine says of this track's broken samples: how many its readouts measured
  // around, and its lane and, in Compare, the sum's, as holding some; in Single, bit 0.
  private invalidFields(
    multi: boolean,
    comparing: boolean,
  ): { invalidSamples: number; invalidLanes: number } {
    if (this.brokenSamples === 0) {
      return { invalidSamples: 0, invalidLanes: 0 };
    }
    const tracks = this.groupTracks();
    const own = tracks.findIndex((track) => track.own);
    const lanes = multi ? 2 ** own + (comparing ? 2 ** tracks.length : 0) : 1;
    return { invalidSamples: this.brokenSamples, invalidLanes: lanes };
  }

  private groupTracks(): typeof mockTracks {
    return this.setting("group") === "a"
      ? mockTracks
      : mockTracks.filter((track) => track.own);
  }

  // The ids of the tracks compared in the Compare view: this one and the others asked for (until
  // asked, the group's first other track), or none when the group has no other track.
  private comparedIds(): number[] {
    if (this.setting("displayMode") !== "compare") {
      return [];
    }
    const others = this.groupTracks().filter((track) => !track.own);
    const asked = others
      .filter(
        (track) => this.compared?.includes(track.id) ?? track === others[0],
      )
      .map((track) => track.id);
    const own = mockTracks.find((track) => track.own)?.id ?? 0;
    return others.length === 0 ? [] : [own, ...asked];
  }

  private sendLanes(): void {
    const compared = this.comparedIds();
    const key = `${String(this.setting("group"))}/${compared.join(",")}`;
    if (key === this.lanesKey) {
      return;
    }
    this.lanesKey = key;
    this.reply({
      type: "lanes",
      laneSet: ++this.laneSet,
      lanes: this.groupTracks().map(({ id, name, colour, own }) => ({
        id,
        name,
        colour,
        own,
        compared: compared.includes(id),
      })),
      hidden: 0,
    });
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

  // The trigger point of a live triggered window of `span` at `time`: the last period
  // boundary a span back.
  private triggerTime(time: number, span: number): number {
    const period = 1 / mockFundamental;
    return Math.floor((time - span) / period) * period;
  }

  // Where a live Seconds window of `span` at `time` starts: triggered spans one trigger offset
  // before a period boundary, roll spans ending now.
  private liveStart(time: number, span: number): number {
    return span > rollThresholdSeconds
      ? time - span
      : this.triggerTime(time, span) - triggerPosition * span;
  }

  // A frozen Seconds view, placed as the engine places it (Analyzer.cpp): the last live window's
  // trigger point stays put, or its right edge when the span now rolls (the newest audio held
  // when there was no live window), and the view reaches back to the oldest of the
  // frozenHoldSeconds of audio held.
  private frozenWindow(span: number): { start: number; reach: number } {
    const frozenAt = this.frozenAt ?? 0;
    const roll = span > rollThresholdSeconds;
    const live = this.lastLive;
    const anchor =
      live === undefined
        ? frozenAt
        : live.start + (roll ? live.span : triggerPosition * live.span);
    const offset = roll || live === undefined ? span : triggerPosition * span;
    const unpanned = anchor - offset;
    const earliest = frozenAt - frozenHoldSeconds;
    return {
      start: Math.min(
        Math.max(earliest, frozenAt - span),
        Math.max(earliest, unpanned - this.panSeconds),
      ),
      reach: Math.max(0, unpanned - earliest),
    };
  }

  private panReach(): number {
    if (this.frozenAt === undefined || this.setting("timeBase") === "beats") {
      return 0;
    }
    return this.frozenWindow(this.setting("timeSpan") as number).reach;
  }

  private sendFrame(): void {
    if (this.columns === 0) {
      return;
    }
    // A pan keeps within the reach, which shrinks when the span grows.
    this.panSeconds = clampPan(this.panSeconds, this.panReach());
    const time = this.frozenAt ?? this.now();
    const synced = this.setting("timeBase") === "beats";
    let span = this.setting("timeSpan") as number;
    let timeOf: (fraction: number) => number;
    let cursor: number | undefined;
    let cycleStart = 0;
    let spanQuarterNotes = 0;
    const roll = !synced && span > rollThresholdSeconds;
    if (synced) {
      // A transport playing at the mock tempo since start-up.
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
      const start =
        this.frozenAt !== undefined
          ? this.frozenWindow(span).start
          : this.liveStart(time, span);
      timeOf = (fraction) => start + fraction * span;
    }

    const visibleWidth = this.view.end - this.view.start;
    const sampleCount = Math.max(
      1,
      Math.round(span * visibleWidth * this.sampleRate),
    );
    const sampleTime = (sample: number) =>
      timeOf(this.view.start + (sample / sampleCount) * visibleWidth);
    const minMax = sampleCount > this.columns;
    const pointCount = minMax ? this.columns : sampleCount;
    const samplesPerPoint = sampleCount / pointCount;
    const points = (signalAt: (time: number) => number): Float32Array => {
      const values = new Float32Array(minMax ? pointCount * 2 : pointCount);
      for (let point = 0; point < pointCount; point++) {
        if (!minMax) {
          values[point] = signalAt(sampleTime(point));
          continue;
        }
        let low = Infinity;
        let high = -Infinity;
        const first = Math.floor(point * samplesPerPoint);
        const last = Math.floor((point + 1) * samplesPerPoint);
        // Long spans: sample each column sparsely, enough for a mock.
        const stride = Math.max(1, Math.floor((last - first) / 64));
        for (let sample = first; sample < last; sample += stride) {
          const value = signalAt(sampleTime(sample));
          low = Math.min(low, value);
          high = Math.max(high, value);
        }
        values[point * 2] = low;
        values[point * 2 + 1] = high;
      }
      return values;
    };

    // The multi-track view draws the group's tracks; readouts measure this one. The Compare view
    // adds the sum of the compared tracks.
    const multi = this.setting("displayMode") !== "single";
    const comparedIds = this.comparedIds();
    const compared = mockTracks.filter((track) =>
      comparedIds.includes(track.id),
    );
    const views = new Map<ChannelView, Float32Array>();
    const lanes: Float32Array[] = [];
    // The readouts measure what the engine's do: the trigger source, or the Mid in Beats mode.
    const mono = this.inputChannels === 1;
    // A mono track measures L, whatever source was asked for, as the engine's readView does.
    const source = mono
      ? ChannelView.left
      : readoutView(synced, this.setting("triggerSource") as ViewName);
    let readoutSignal = (time: number) => viewSample(source, time, mono);
    let comparison: FrameContent["comparison"];
    if (multi) {
      for (const track of this.groupTracks()) {
        lanes.push(points(track.sample));
      }
      readoutSignal = kick;
    }
    if (compared.length > 0) {
      const sum = (fix: boolean) => (time: number) =>
        compared.reduce(
          (total, track) =>
            total + (fix ? (track.fixed ?? track.sample) : track.sample)(time),
          0,
        );
      lanes.push(points(sum(this.previewFix)));
      // Summing gains over the window, from a few thousand samples.
      const gain = (fix: boolean) => {
        let together = 0;
        let apart = 0;
        const stride = Math.max(1, Math.floor(sampleCount / 4096));
        for (let sample = 0; sample < sampleCount; sample += stride) {
          const time = sampleTime(sample);
          together += sum(fix)(time) ** 2;
          for (const track of compared) {
            apart +=
              (fix ? (track.fixed ?? track.sample) : track.sample)(time) ** 2;
          }
        }
        return mockSummingGain(together, apart);
      };
      comparison = {
        sumGain: gain(false),
        fixedGain: gain(true),
        matches: this.groupTracks().map((track) =>
          track.own || !comparedIds.includes(track.id)
            ? noMatch
            : track.match === undefined
              ? unrelated
              : {
                  ...track.match,
                  offset: Math.round((track.delay ?? 0) * this.sampleRate),
                },
        ),
      };
    } else if (!multi) {
      // A mono track shows L alone, whatever was asked for.
      const names = mono
        ? ["left"]
        : (this.setting("channelViews") as readonly string[]);
      for (const [name, view] of Object.entries(ChannelView)) {
        if (names.includes(name)) {
          views.set(
            view,
            points((time) => viewSample(view, time, mono)),
          );
        }
      }
    }

    let sumOfSquares = 0;
    let peak = 0;
    const readoutStride = Math.max(1, Math.floor(sampleCount / 4096));
    let readoutSamples = 0;
    for (let sample = 0; sample < sampleCount; sample += readoutStride) {
      const value = readoutSignal(sampleTime(sample));
      sumOfSquares += value * value;
      peak = Math.max(peak, Math.abs(value));
      readoutSamples++;
    }

    this.frameHandler?.(
      encodeFrame({
        mode: minMax ? FrameMode.minMax : FrameMode.raw,
        triggered: !roll && !synced,
        frozen: this.frozenAt !== undefined,
        panReach: this.panReach(),
        sampleRate: this.sampleRate,
        roll,
        synced,
        cursor,
        cycleStart,
        cycleSeconds: (cycleStart * 60) / mockTempo,
        spanQuarterNotes,
        barQuarterNotes: synced ? mockBarQuarterNotes : 0,
        viewStart: this.view.start,
        viewEnd: this.view.end,
        visibleSamples: sampleCount,
        pointCount,
        startFraction: 0,
        peak: readoutSamples > 0 ? peak : Number.NaN,
        rms:
          readoutSamples > 0
            ? Math.sqrt(sumOfSquares / readoutSamples)
            : Number.NaN,
        frequency: multi ? undefined : mockFundamental,
        views,
        lanes,
        laneSet: multi ? this.laneSet : 0,
        ...this.invalidFields(multi, comparison !== undefined),
        ...(comparison === undefined ? {} : { comparison }),
      }),
    );
  }
}
