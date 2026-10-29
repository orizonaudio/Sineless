// A back end that replays what the real engine sent: a session recorded by the editor tests
// (editor/tests/SessionRecordingTests.cpp) in protocol/tests/data/sessions. The page gets the
// recorded snapshot, the session's answers and replies, and each phase's frame, so it is tested on
// what only the engine decides. It may send only the messages the recording holds: one it does
// not is collected, and the harness fails the test with it.
import { decodeFrame } from "../../../protocol/src/frame-decoder.ts";
import type { ControlMessage, Transport } from "../../src/bridge/transport.ts";

export interface RecordedPhase {
  // The page's message that starts the phase, or the host's event ({host, rate}); null: hello.
  readonly after: Readonly<Record<string, unknown>> | null;
  // What the session sent at once in answer to `after`.
  readonly answers: readonly ControlMessage[];
  // What the session sent later, at its syncs (lanes, a new snapshot after a host event).
  readonly replies: readonly ControlMessage[];
  // The frame the phase ends on, in base64; none when the engine drew none (a rate change while
  // frozen).
  readonly frames: readonly string[];
}

export interface Recording {
  readonly scenario: string;
  readonly protocolVersion: number;
  // The page the frames were recorded for (its width in CSS pixels and its pixel ratio).
  readonly page: { readonly viewWidth: number; readonly pixelRatio: number };
  readonly snapshot: ControlMessage;
  readonly phases: readonly RecordedPhase[];
}

// A message the recording does not hold, and what it expected next.
export interface Unexpected {
  readonly message: ControlMessage;
  readonly expected: RecordedPhase["after"] | undefined;
}

// How far a `view` or `pan` the page sends may be from the recorded one: of the view's width, of
// the span. The page computes them in floating point, so they are not compared exactly; a real
// difference (another zoom step, another swipe) is far larger.
export const replayTolerance = 1e-6;

// Messages that say nothing the recording depends on: the frames keep the columns recorded for
// the page's width at hello, which the replay checks (editor/tests/SessionRecordingTests.cpp,
// kPageWidth).
const ignored = new Set(["viewport"]);
// Messages the page sends on its way to a recorded one: each wheel event of a zoom, each swipe.
const steps = new Set(["view", "pan"]);

function bytesOf(base64: string): ArrayBuffer {
  const text = atob(base64);
  const bytes = new Uint8Array(text.length);
  for (let i = 0; i < text.length; i++) {
    bytes[i] = text.charCodeAt(i);
  }
  return bytes.buffer;
}

export class ReplayTransport implements Transport {
  // Messages the recording does not hold, in order; the harness fails the test on any.
  readonly unexpected: Unexpected[] = [];
  // `view` and `pan` messages that did not start a phase: steps on the way to a recorded one, or
  // the last one sent again.
  readonly steps: ControlMessage[] = [];

  private readonly recording: Recording;
  private readonly schedule: (work: () => void) => void;
  private messageHandler: ((message: ControlMessage) => void) | undefined;
  private frameHandler: ((frame: ArrayBuffer) => void) | undefined;
  private phase = -1; // Before hello.
  private repliesDue = false;
  // A step sent since the last phase began, which the page must still reach the next phase from:
  // a test that ends there sent a view or pan the recording does not hold.
  private pending: Unexpected | undefined;

  constructor(
    recording: Recording,
    schedule: (work: () => void) => void = (work) => setTimeout(work, 0),
  ) {
    this.recording = recording;
    this.schedule = schedule;
  }

  // The phase the replay is in: its index in the recording.
  get currentPhase(): number {
    return this.phase;
  }

  onMessage(handler: (message: ControlMessage) => void): void {
    this.messageHandler = handler;
  }

  onFrame(handler: (frame: ArrayBuffer) => void): void {
    this.frameHandler = handler;
  }

  send(message: ControlMessage): void {
    if (ignored.has(message.type)) {
      return;
    }
    if (message.type === "hello") {
      const { page } = this.recording;
      if (
        message["viewWidth"] !== page.viewWidth ||
        message["pixelRatio"] !== page.pixelRatio
      ) {
        // The frames have the columns of another page.
        this.unexpected.push({ message, expected: page });
      }
      this.deliver([this.recording.snapshot]);
      this.enter(0);
      return;
    }
    const next = this.recording.phases[this.phase + 1];
    if (
      next?.after !== null &&
      next !== undefined &&
      matches(message, next.after, this.span())
    ) {
      this.enter(this.phase + 1);
      return;
    }
    if (steps.has(message.type)) {
      this.steps.push(message);
      const current = this.recording.phases[this.phase]?.after;
      if (
        next !== undefined &&
        next.after !== null &&
        next.after["host"] === undefined
      ) {
        this.pending = { message, expected: next.after };
      } else if (
        current === null ||
        current === undefined ||
        !matches(message, current, this.span())
      ) {
        // Nothing further is recorded: only the last view or pan may be sent again.
        this.unexpected.push({ message, expected: next?.after });
      }
      return;
    }
    this.unexpected.push({ message, expected: next?.after });
  }

  // The view or pan the page stopped at without reaching the next recorded one, once; called when
  // a test ends.
  takePending(): Unexpected | undefined {
    const pending = this.pending;
    this.pending = undefined;
    return pending;
  }

  // The host's event that starts the next phase (a rate change), when the test says it happens.
  hostEvent(): void {
    const next = this.recording.phases[this.phase + 1];
    if (next?.after?.["host"] === undefined) {
      throw new Error(
        `${this.recording.scenario}: the next phase is not a host event (${JSON.stringify(next?.after)})`,
      );
    }
    this.enter(this.phase + 1);
  }

  requestFrame(): void {
    this.schedule(() => {
      const frame = this.frame();
      if (frame === undefined) {
        return;
      }
      this.frameHandler?.(bytesOf(frame));
      if (this.repliesDue) {
        this.repliesDue = false;
        this.messages(this.recording.phases[this.phase]?.replies ?? []);
      }
    });
  }

  // The frame the page gets now: the phase's, or, in a phase with none, the last one before it, as
  // the page keeps the frame it has.
  private frame(): string | undefined {
    for (let index = this.phase; index >= 0; index--) {
      const frames = this.recording.phases[index]?.frames ?? [];
      const last = frames[frames.length - 1];
      if (last !== undefined) {
        return last;
      }
    }
    return undefined;
  }

  // The span the current frame shows, in seconds, which a `pan` is compared against.
  private span(): number {
    const frame = this.frame();
    const span =
      frame === undefined ? 0 : decodeFrame(bytesOf(frame)).spanSeconds;
    return span > 0 ? span : 1;
  }

  private enter(index: number): void {
    this.phase = index;
    this.pending = undefined;
    const phase = this.recording.phases[index];
    if (phase === undefined) {
      return;
    }
    this.messages(phase.answers);
    // Replies follow the phase's first frame, as the session's sync follows the frame; a phase
    // with no frame gets them at once.
    if (phase.frames.length > 0) {
      this.repliesDue = true;
    } else {
      this.repliesDue = false;
      this.messages(phase.replies);
    }
  }

  private messages(messages: readonly ControlMessage[]): void {
    if (messages.length > 0) {
      this.deliver(messages);
    }
  }

  private deliver(messages: readonly ControlMessage[]): void {
    this.schedule(() => {
      for (const message of messages) {
        this.messageHandler?.(message);
      }
    });
  }
}

// Whether the page's `message` is the recorded one: the same type and fields, numbers exactly but
// for `view` (within replayTolerance of its width) and `pan` (of the span).
export function matches(
  message: ControlMessage,
  recorded: Readonly<Record<string, unknown>>,
  spanSeconds: number,
): boolean {
  const keys = Object.keys(message);
  if (
    keys.length !== Object.keys(recorded).length ||
    message.type !== recorded["type"]
  ) {
    return false;
  }
  const start = recorded["start"];
  const end = recorded["end"];
  const scale =
    message.type === "view" &&
    typeof start === "number" &&
    typeof end === "number"
      ? end - start
      : message.type === "pan"
        ? spanSeconds
        : 0;
  return keys.every((key) => {
    const value = message[key];
    const expected = recorded[key];
    if (
      typeof value === "number" &&
      typeof expected === "number" &&
      scale > 0
    ) {
      return Math.abs(value - expected) <= replayTolerance * scale;
    }
    return JSON.stringify(value) === JSON.stringify(expected);
  });
}

// The recordings, by scenario name, as Vite finds them.
const files = import.meta.glob<Recording>(
  "../../../protocol/tests/data/sessions/*.json",
  { import: "default" },
);

// The recording of `scenario`; fails saying how to write it when there is none.
export async function loadRecording(
  scenario: string,
  available: Readonly<Record<string, () => Promise<Recording>>> = files,
): Promise<Recording> {
  const path = `../../../protocol/tests/data/sessions/${scenario}.json`;
  const load = available[path];
  if (load === undefined) {
    throw new Error(
      `protocol/tests/data/sessions/${scenario}.json is missing: add the scenario to ` +
        "editor/tests/SessionRecordingTests.cpp and run the editor tests with " +
        "ORIZON_UPDATE_RECORDINGS=1",
    );
  }
  return load();
}

// What the failure says about each unexpected message.
export function describeUnexpected(
  scenario: string,
  unexpected: readonly Unexpected[],
): string {
  return unexpected
    .map(
      ({ message, expected }) =>
        `${scenario}: the page sent ${JSON.stringify(message)}, which the recording does not ` +
        `hold; it expected ${expected === undefined ? "nothing more" : JSON.stringify(expected)}`,
    )
    .join("\n");
}
