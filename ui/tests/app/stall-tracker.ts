// Which browser projects of the app tests have stopped making progress (stall-watchdog.ts). In
// vitest browser mode a test's timeout runs in the page: a page whose main thread stops never
// fires it, and nothing on Node notices, so the run waits for ever with no output. Each event a
// project's page sends (a file queued, collected, started or ended; a test or hook started or
// done) is its progress; a project silent for its limit has stalled. Pure: the clock is given.

// vitest's own default for a browser project's test timeout, for a project that has none.
const fallbackTestTimeoutMs = 30_000;

// How long a project may send nothing: `override` (ORIZON_APP_STALL_SECONDS) when it is a
// finite positive number of seconds, else four times the project's test timeout. Every wait in
// a test or hook is bounded by that timeout, and a file's import and collection take seconds,
// so a healthy page sends something well within it.
export function stallLimitMs(
  override: string | undefined,
  testTimeoutMs: number,
): number {
  const seconds = Number(override?.trim() || Number.NaN);
  if (Number.isFinite(seconds) && seconds > 0) {
    return seconds * 1000;
  }
  const timeout =
    Number.isFinite(testTimeoutMs) && testTimeoutMs > 0
      ? testTimeoutMs
      : fallbackTestTimeoutMs;
  return 4 * timeout;
}

export interface WatchedProject {
  // The test files the project will run.
  readonly files: number;
  readonly limitMs: number;
}

export interface Stall {
  readonly project: string;
  readonly report: string;
}

interface ProjectState extends WatchedProject {
  ended: number;
  // From the project's first event until its last file ends: a project that has not begun
  // (waiting behind the other browser) has sent nothing, and is not stalled.
  armed: boolean;
  last: { readonly at: number; readonly what: string };
  reported: boolean;
}

export class StallTracker {
  private projects = new Map<string, ProjectState>();

  // A run begins: the projects it runs, by name.
  start(projects: ReadonlyMap<string, WatchedProject>): void {
    this.projects = new Map(
      [...projects].map(([name, project]) => [
        name,
        {
          ...project,
          ended: 0,
          armed: false,
          last: { at: 0, what: "" },
          reported: false,
        },
      ]),
    );
  }

  // The project's page sent something (`what`, as the report will name it) at `at`.
  progress(project: string, what: string, at: number): void {
    const state = this.projects.get(project);
    if (state === undefined) {
      return;
    }
    state.armed = state.ended < state.files;
    state.last = { at, what };
  }

  // One of the project's files ended: progress too, and the project's last once all have.
  fileEnded(project: string, what: string, at: number): void {
    const state = this.projects.get(project);
    if (state === undefined) {
      return;
    }
    state.ended++;
    this.progress(project, what, at);
  }

  // The projects stalled at `at`, each reported once.
  check(at: number): Stall[] {
    const stalls: Stall[] = [];
    for (const [project, state] of this.projects) {
      const silent = at - state.last.at;
      if (!state.armed || state.reported || silent < state.limitMs) {
        continue;
      }
      state.reported = true;
      stalls.push({
        project,
        report:
          `${project}: no progress for ${String(Math.round(silent / 1000))} s since ` +
          `${state.last.what}: its browser page stopped answering, and the tests' own timeouts, ` +
          `which run in the page, cannot fire`,
      });
    }
    return stalls;
  }

  // The run ended: nothing stalls after it.
  stop(): void {
    this.projects.clear();
  }
}
