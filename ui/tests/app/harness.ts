// Starts the real page, as the plugin's web view loads it, on the mock back end. Each test file
// runs in a page of its own. The app finds its elements by id, so an app started earlier in the
// same file would write into the new one's page: starting an app retires the one before, as a
// closed editor window. The retired app hears nothing from its back end and sends it nothing, the
// timers, listeners and size observers it set up as it started are removed, and a draw it had
// queued runs before the page is replaced; the elements it listened on are gone with its page.
import {
  afterEach,
  beforeEach,
  expect,
  inject,
  type RunnerTestCase,
  TestRunner,
  vi,
} from "vitest";
import { userEvent } from "vitest/browser";

import html from "../../index.html?raw";
import {
  MockTransport,
  type MockOptions,
} from "../../src/bridge/mock-transport.ts";
import type { ControlMessage, Transport } from "../../src/bridge/transport.ts";
import { ticksPerFrame } from "../../src/render/render-loop.ts";
import { startSineless } from "../../src/start.ts";
import "../../src/styles.css";
import {
  describeUnexpected,
  loadRecording,
  ReplayTransport,
} from "./replay-transport.ts";

export interface Started<Back extends Transport = MockTransport> {
  readonly transport: Back;
  // Messages the page sent, in order.
  readonly sent: ControlMessage[];
  // Holds the page's request for a frame, as the engine does while nothing triggers, and
  // answers it when resumed.
  holdFrames(held: boolean): void;
  // Delivers a message to the page as if the engine had sent it.
  deliver(message: ControlMessage): void;
}

// Starts the page on a new mock back end, or on `transport`: the page reopened on the same
// instance, as when the editor window is closed and opened again.
export async function startApp(
  options: MockOptions = {},
  transport = new MockTransport(options),
): Promise<Started> {
  return startOn(transport);
}

// Starts the page on a new mock back end with its requests for frames held, so that it shows
// what it shows before the engine's first frame, until holdFrames(false).
export async function startBeforeFrames(
  options: MockOptions = {},
): Promise<Started> {
  return startOn(new MockTransport(options), true);
}

// The replay the page was started on by startRecorded, which fails the test on a message its
// recording does not hold.
let replay: { scenario: string; transport: ReplayTransport } | undefined;

// Fails with the messages the page sent that the recording does not hold, and forgets them; at
// the end of a test, also with a view or pan it stopped at short of the next recorded one.
export function checkReplay(endOfTest = false): void {
  if (replay === undefined) {
    return;
  }
  const pending = endOfTest ? replay.transport.takePending() : undefined;
  if (pending !== undefined) {
    replay.transport.unexpected.push(pending);
  }
  if (replay.transport.unexpected.length === 0) {
    return;
  }
  const failure = describeUnexpected(
    replay.scenario,
    replay.transport.unexpected,
  );
  replay.transport.unexpected.length = 0;
  throw new Error(failure);
}

afterEach(() => {
  checkReplay(true);
});

// Starts the page on a session the real engine recorded (protocol/tests/data/sessions): it is
// shown what the engine sent, and may send only what the recording holds.
export async function startRecorded(
  scenario: string,
): Promise<Started<ReplayTransport>> {
  const transport = new ReplayTransport(await loadRecording(scenario));
  replay = { scenario, transport };
  return startOn(transport);
}

// Cuts the app started last off from its back end and removes what it set up as it started.
let retirePrevious: () => void = () => undefined;

declare module "vitest" {
  export interface ProvidedContext {
    ci: boolean;
  }
}

// How long the start waits for the page's animation frames: long enough that a stall which ends
// within it passes, as it would with no limit, and short enough to leave half of a hook's 30 s for
// the rest of a setup. Tests of the wait itself shorten it.
let queuedBudget = 15000;

export function setFrameBudgetForTests(budget = 15000): void {
  queuedBudget = budget;
}

// The note of the stall that skipped a test last, which beforeApp reads to tell a skip from a
// failure: vitest does not export the error a skip throws.
let lastStallNote: string | undefined;

function stallNoteSinceReset(): string | undefined {
  return lastStallNote;
}

// Whether the tests run on CI (vitest.config.ts), or as a test of the skip sets it.
let ciForTests: boolean | undefined;

export function setCiForTests(ci?: boolean): void {
  ciForTests = ci;
}

// The next animation frame's time, or undefined once `within` ms pass without one: a page whose
// rendering has stopped (the CI machine's display stalling) gets none, while its timers run.
export function nextFrame(within: number): Promise<number | undefined> {
  return new Promise((resolve) => {
    let done = false;
    const timer = setTimeout(() => {
      done = true;
      resolve(undefined);
    }, within);
    requestAnimationFrame((at) => {
      if (!done) {
        done = true;
        clearTimeout(timer);
        resolve(at);
      }
    });
  });
}

// What the page looked like when its frames stopped coming, for the failure: whether it is
// hidden, out of focus or sized to nothing.
// `frames`: those the wait got before they stopped, when it waited for several.
function stallNote(waitedFor: string, frames?: number): string {
  const before =
    frames === undefined ? "" : `${String(frames)} before it stopped; `;
  return (
    `the page got no animation frame in ${waitedFor} (${before}` +
    `page ${document.visibilityState}, ${document.hasFocus() ? "focused" : "not focused"}, ` +
    `${String(window.innerWidth)} × ${String(window.innerHeight)}): its rendering stalled`
  );
}

// The page's rendering stopped. The CI machine's display stops driving it for seconds now and then,
// which no host's display does: there the test running is skipped, saying so. Off CI it fails, so a
// page that stopped rendering still shows on a developer's machine, and so does a start outside a
// test (a beforeAll), which has no test to skip.
function stalled(waitedFor: string, frames?: number): never {
  const note = stallNote(waitedFor, frames);
  const running = TestRunner.getCurrentTest<RunnerTestCase | undefined>();
  if ((ciForTests ?? inject("ci")) && running) {
    lastStallNote = note;
    running.context.skip(note);
  }
  throw new Error(note);
}

// An animation frame, waited for within `within` ms; the test stops if none comes.
export async function frame(
  waitedFor = "a frame",
  within = 5000,
): Promise<number> {
  const at = await nextFrame(within);
  if (at === undefined) {
    stalled(waitedFor);
  }
  return at;
}

// Runs `setUp` (one that starts the app) once for the tests of the block it is in, as beforeAll
// does, but at its first test, where a stalled start can skip: vitest skips nothing from a
// beforeAll. Every test of the block then has the setup's outcome: run, skipped with its note, or
// failed with its error.
export function beforeApp(setUp: () => unknown): void {
  let outcome:
    | Promise<
        | { readonly kind: "done" }
        | { readonly kind: "skipped"; readonly note: string }
        | { readonly kind: "failed"; readonly error: unknown }
      >
    | undefined;
  beforeEach(async (context) => {
    outcome ??= (async () => {
      lastStallNote = undefined;
      try {
        await setUp();
        return { kind: "done" } as const;
      } catch (error) {
        const note = stallNoteSinceReset();
        return note === undefined
          ? ({ kind: "failed", error } as const)
          : ({ kind: "skipped", note } as const);
      }
    })();
    const result = await outcome;
    if (result.kind === "skipped") {
      context.skip(result.note);
    }
    if (result.kind === "failed") {
      throw result.error;
    }
  });
}

// Waits out the ticks a retired app's draw loop already asked for, which draw into the page by id:
// a frame waiting is drawn within n ticks of the last, n for the browser's tick rate, so two ticks
// more than that, measured from the shortest gap seen (at most a 1000 Hz rate's), within the
// start's budget.
async function queuedDrawsRun(): Promise<void> {
  const deadline = performance.now() + queuedBudget;
  let last: number | undefined;
  let shortest = Number.POSITIVE_INFINITY;
  let frames = 0;
  const ticks = () =>
    2 + Math.min(ticksPerFrame(1000 / shortest), ticksPerFrame(1000));
  for (let tick = 0; tick < ticks(); tick++) {
    const at = await nextFrame(Math.max(0, deadline - performance.now()));
    if (at === undefined) {
      stalled("the ticks the previous app asked for", frames);
    }
    frames++;
    if (last !== undefined) {
      shortest = Math.min(shortest, at - last);
    }
    last = at;
  }
}

// Runs `start`, recording the listeners, timers and size observers it sets up (the app sets up
// all of its window and document ones as it starts), and returns what removes them. An observer
// left on would see its screen detached when the page is replaced, as a change to no size.
function recordSetUp(start: () => void): () => void {
  const listening = vi.spyOn(EventTarget.prototype, "addEventListener");
  const intervals = vi.spyOn(window, "setInterval");
  const timeouts = vi.spyOn(window, "setTimeout");
  const observing = vi.spyOn(ResizeObserver.prototype, "observe");
  let listeners: (readonly [
    EventTarget,
    Parameters<EventTarget["addEventListener"]>,
  ])[] = [];
  let timers: number[] = [];
  let observers: ResizeObserver[] = [];
  try {
    start();
  } finally {
    // Read before restoring, which forgets the calls.
    listeners = listening.mock.calls.map(
      (call, index) =>
        [listening.mock.contexts[index] as EventTarget, call] as const,
    );
    timers = [...intervals.mock.results, ...timeouts.mock.results].map(
      (result) => result.value as number,
    );
    observers = observing.mock.contexts as ResizeObserver[];
    listening.mockRestore();
    intervals.mockRestore();
    timeouts.mockRestore();
    observing.mockRestore();
  }
  return () => {
    for (const [target, [type, listener, options]] of listeners) {
      target.removeEventListener(type, listener, options);
    }
    for (const id of timers) {
      // One id space for both: clearTimeout clears an interval too.
      window.clearTimeout(id);
    }
    for (const observer of observers) {
      observer.disconnect();
    }
  };
}

async function startOn<Back extends Transport>(
  transport: Back,
  framesHeld = false,
): Promise<Started<Back>> {
  retirePrevious();
  await queuedDrawsRun();
  let retired = false;
  let removeSetUp: () => void = () => undefined;
  retirePrevious = () => {
    retired = true;
    removeSetUp();
  };
  document.body.innerHTML = new DOMParser().parseFromString(
    html,
    "text/html",
  ).body.innerHTML;
  const sent: ControlMessage[] = [];
  let held = framesHeld;
  let waiting = false; // A request held back.
  let deliver: (message: ControlMessage) => void = () => undefined;
  const watched: Transport = {
    onMessage(handler) {
      deliver = handler;
      transport.onMessage((message) => {
        if (!retired) {
          handler(message);
        }
      });
    },
    onFrame(handler) {
      transport.onFrame((frame) => {
        if (!retired) {
          handler(frame);
        }
      });
    },
    send(message) {
      if (retired) {
        return;
      }
      sent.push(message);
      transport.send(message);
    },
    requestFrame() {
      if (retired) {
        return;
      }
      if (held) {
        waiting = true;
      } else {
        transport.requestFrame();
      }
    },
  };
  removeSetUp = recordSetUp(() => {
    startSineless(watched);
  });
  await vi.waitFor(
    () => {
      if (framesHeld) {
        expect(waiting).toBe(true);
      } else {
        expect(window.orizonDebug?.frameCount ?? 0).toBeGreaterThan(0);
      }
    },
    { timeout: 5000 },
  );
  return {
    transport,
    sent,
    holdFrames(value) {
      held = value;
      if (!held && waiting) {
        waiting = false;
        transport.requestFrame();
      }
    },
    deliver(message) {
      deliver(message);
    },
  };
}

// The element with this id; fails the test when the page has none.
export function byId(id: string): HTMLElement {
  const found = document.getElementById(id);
  expect(found, `#${id}`).not.toBeNull();
  return found as HTMLElement;
}

// A button of a segmented control, by its label.
export function choice(groupId: string, label: string): HTMLButtonElement {
  const button = [...byId(groupId).querySelectorAll("button")].find(
    (candidate) => candidate.firstChild?.textContent === label,
  );
  expect(button, `${groupId} › ${label}`).toBeDefined();
  return button as HTMLButtonElement;
}

// Clicks a control, first opening the toolbar popover it sits in (Group, Views, Trigger) if that
// is closed, as a user would.
// The span pill each time base's button shows.
export const spanFieldOf: Readonly<Record<string, string>> = {
  Beats: "beat-span-field",
  Free: "free-span-field",
  Trig: "span-field",
};

// Switches the time base and waits until the page shows it: until then the controls of that time
// base (its span pill, Trig's trigger source) may still be hidden, and a hidden control takes
// no press.
export async function inTimeBase(timeBase: string): Promise<void> {
  await userEvent.click(choice("time-base", timeBase));
  await expect
    .element(byId(spanFieldOf[timeBase] ?? "span-field"))
    .toBeVisible();
}

export async function press(button: HTMLElement): Promise<void> {
  const popover = button.closest<HTMLElement>(".popover");
  if (popover?.hidden === true) {
    await userEvent.click(byId(popover.id.replace(/-popover$/, "-details")));
  }
  await userEvent.click(button);
}

// Presses a toggle button, or releases it, unless it is already so.
export async function setPressed(
  button: HTMLElement,
  pressed: boolean,
): Promise<void> {
  if ((button.getAttribute("aria-pressed") === "true") !== pressed) {
    await userEvent.click(button);
  }
  await expect.element(button).toHaveAttribute("aria-pressed", String(pressed));
}

// Shows the views whose toggles are labelled `shown` ("L", "M") and hides the others, then waits
// for a frame drawing them.
export async function showViews(shown: readonly string[]): Promise<void> {
  if (byId("views-popover").hidden) {
    await userEvent.click(byId("views-details"));
  }
  const toggles = [...byId("views").querySelectorAll("button")];
  // Shown ones first, so that one view stays shown throughout.
  for (const button of toggles) {
    if (shown.includes(button.textContent)) {
      await setPressed(button, true);
    }
  }
  for (const button of toggles) {
    if (!shown.includes(button.textContent)) {
      await setPressed(button, false);
    }
  }
  // The toggle is named after the views a frame has drawn, so its name changes a frame after the
  // last toggle: close it once that frame has come, not while the click's locator can go stale.
  const drawn = toggles
    .map((button) => button.textContent)
    .filter((label) => shown.includes(label));
  await expect
    .element(byId("views-details"))
    .toHaveAttribute("aria-label", `Views: ${drawn.join(" ")}`);
  await userEvent.click(byId("views-details"));
}

// Chooses the channel the readouts measure in the status bar's popover ("S"), and closes it.
export async function useReadout(label: string): Promise<void> {
  await press(choice("readout-source", label));
  await expect
    .element(choice("readout-source", label))
    .toHaveAttribute("aria-pressed", "true");
  await userEvent.click(byId("readout-details"));
}

// Waits until the page has drawn frames that reflect everything asked so far.
export async function settle(frames = 3): Promise<void> {
  checkReplay();
  const from = window.orizonDebug?.frameCount ?? 0;
  await vi.waitFor(
    () => {
      expect(window.orizonDebug?.frameCount ?? 0).toBeGreaterThanOrEqual(
        from + frames,
      );
    },
    { timeout: 5000 },
  );
}

// Types `text` into a number field, as a double-click and Enter do.
export async function typeInto(hostId: string, text: string): Promise<void> {
  const field = byId(hostId).querySelector<HTMLElement>(".field");
  expect(field, `${hostId} field`).not.toBeNull();
  await userEvent.dblClick(field as HTMLElement);
  const input = (field as HTMLElement).querySelector("input");
  expect(input, `${hostId} input`).not.toBeNull();
  await userEvent.fill(input as HTMLInputElement, text);
  await userEvent.keyboard("{Enter}");
}

// The status chips' texts, in order, those folded into "+n" included.
export function chips(): string[] {
  return [...byId("chips").querySelectorAll(".chip")].map(
    (chip) => chip.textContent,
  );
}

// The status chips shown in the status bar itself, in order: none folded into "+n".
export function shownChips(): string[] {
  return [...byId("chips").children]
    .filter((child) => child.classList.contains("chip"))
    .map((chip) => chip.textContent);
}

// A horizontal swipe over the traces of `screenWidths` (negative: back in time).
export function swipe(screenWidths: number): void {
  const screen = byId("screen");
  byId("trace").dispatchEvent(
    new WheelEvent("wheel", {
      deltaX: screenWidths * screen.clientWidth,
      deltaMode: WheelEvent.DOM_DELTA_PIXEL,
      bubbles: true,
      cancelable: true,
    }),
  );
}
