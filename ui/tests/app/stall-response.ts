// What the stall watchdog (stall-watchdog.ts) does once a browser project has stalled: say so,
// say what the browser processes were doing, and close that project's browser, which makes vitest
// fail its file ("Browser connection was closed") and end the run. The process list is
// best-effort and never stops the close; a browser that does not close (vitest's own close is
// bounded the same way: "a frozen or crashed browser never answers the close message") ends the
// process with failure. Its effects are given, for its test.

export interface StallEffects {
  write(text: string): void;
  // The browser processes, as text.
  list(): Promise<string>;
  // Closes the stalled project's browser.
  close(): Promise<void>;
  exit(code: number): void;
}

export const listTimeoutMs = 5000;
export const closeTimeoutMs = 30_000;

const timedOut = Symbol("timed out");

function within<T>(
  promise: Promise<T>,
  ms: number,
): Promise<T | typeof timedOut> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      resolve(timedOut);
    }, ms);
    promise.then(
      (value) => {
        clearTimeout(timer);
        resolve(value);
      },
      (error: unknown) => {
        clearTimeout(timer);
        reject(error instanceof Error ? error : new Error(String(error)));
      },
    );
  });
}

export async function respondToStall(
  report: string,
  effects: StallEffects,
): Promise<void> {
  effects.write(`\n${report}\n`);
  let listing: string | undefined;
  try {
    const result = await within(effects.list(), listTimeoutMs);
    listing = result === timedOut ? undefined : result;
  } catch {
    listing = undefined;
  }
  effects.write(`${listing ?? "process list unavailable"}\n`);
  try {
    const closed = await within(effects.close(), closeTimeoutMs);
    if (closed === timedOut) {
      effects.write(
        `the browser did not close within ${String(closeTimeoutMs / 1000)} s: ending the run\n`,
      );
      effects.exit(1);
    }
  } catch (error) {
    effects.write(
      `the browser could not be closed (${String(error)}): ending the run\n`,
    );
    effects.exit(1);
  }
}
