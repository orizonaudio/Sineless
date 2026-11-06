import { afterEach, beforeEach, describe, expect, test, vi } from "vitest";

import {
  closeTimeoutMs,
  listTimeoutMs,
  respondToStall,
  type StallEffects,
} from "./app/stall-response.ts";

interface Recorded extends StallEffects {
  readonly events: string[];
}

function effects(
  list: () => Promise<string>,
  close: () => Promise<void>,
): Recorded {
  const events: string[] = [];
  return {
    events,
    write: (text) => events.push(`write ${text.trim()}`),
    list: () => {
      events.push("list");
      return list();
    },
    close: () => {
      events.push("close");
      return close();
    },
    exit: (code) => events.push(`exit ${String(code)}`),
  };
}

const never = <T>() => new Promise<T>(() => undefined);

// A stalled project's browser is always closed, which ends the run with the file failed; the
// report comes first, and nothing about the process list can stop the close. A browser that does
// not close ends the process, so the run never waits on it.
describe("a stalled project", () => {
  beforeEach(() => {
    vi.useFakeTimers();
  });
  afterEach(() => {
    vi.useRealTimers();
  });

  test("is reported, listed and closed, in that order", async () => {
    const run = effects(
      () => Promise.resolve("PID COMMAND\n1 Playwright"),
      () => Promise.resolve(),
    );
    await respondToStall("webkit: no progress", run);
    expect(run.events).toEqual([
      "write webkit: no progress",
      "list",
      "write PID COMMAND\n1 Playwright",
      "close",
    ]);
  });

  test("is closed when the process list fails", async () => {
    const run = effects(
      () => Promise.reject(new Error("no ps")),
      () => Promise.resolve(),
    );
    await respondToStall("webkit: no progress", run);
    expect(run.events).toContain("write process list unavailable");
    expect(run.events.at(-1)).toBe("close");
  });

  test("is closed when the process list never answers, after its time", async () => {
    const run = effects(never, () => Promise.resolve());
    const done = respondToStall("webkit: no progress", run);
    await vi.advanceTimersByTimeAsync(listTimeoutMs - 1);
    expect(run.events).not.toContain("close");
    await vi.advanceTimersByTimeAsync(1);
    await done;
    expect(run.events).toContain("write process list unavailable");
    expect(run.events.at(-1)).toBe("close");
  });

  test("ends the process when its browser does not close in time, and not before", async () => {
    const run = effects(() => Promise.resolve(""), never);
    const done = respondToStall("webkit: no progress", run);
    await vi.advanceTimersByTimeAsync(closeTimeoutMs - 1);
    expect(run.events).not.toContain("exit 1");
    await vi.advanceTimersByTimeAsync(1);
    await done;
    expect(run.events.at(-1)).toBe("exit 1");
  });

  test("ends the process at once when its browser cannot be closed", async () => {
    const run = effects(
      () => Promise.resolve(""),
      () => Promise.reject(new Error("closed already")),
    );
    await respondToStall("webkit: no progress", run);
    expect(run.events.at(-1)).toBe("exit 1");
    expect(run.events.join("\n")).toContain("could not be closed");
  });

  test("does not end the process when its browser closes", async () => {
    const run = effects(
      () => Promise.resolve(""),
      () => Promise.resolve(),
    );
    await respondToStall("webkit: no progress", run);
    expect(run.events).not.toContain("exit 1");
  });
});
