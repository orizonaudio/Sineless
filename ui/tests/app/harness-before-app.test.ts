// beforeApp runs a block's setup once, at its first test, and gives every test of the block the
// setup's outcome: run, skipped when its start stalled on CI, or failed. Each block below reads
// what happened from its own afterAll: a skipped test runs no body to check itself.
import { afterAll, describe, expect, test, vi } from "vitest";

import {
  beforeApp,
  setCiForTests,
  setFrameBudgetForTests,
  startApp,
} from "./harness.ts";

// A page whose rendering has stopped: no animation frames.
function framesStop(): void {
  vi.stubGlobal("requestAnimationFrame", () => 0);
}

function restore(): void {
  vi.unstubAllGlobals();
  setFrameBudgetForTests();
  setCiForTests();
}

interface Outcome {
  readonly state: string | undefined;
  readonly note: string | undefined;
}

// The states of a block's tests, and their skip notes, which vitest's types leave out.
function outcomes(tasks: readonly { result?: unknown }[]): Outcome[] {
  return tasks.map((task) => {
    const result = task.result as { state?: string; note?: string } | undefined;
    return { state: result?.state, note: result?.note };
  });
}

describe("a setup that starts the app", () => {
  let setUps = 0;
  beforeApp(async () => {
    setUps++;
    await startApp();
  });

  // eslint-disable-next-line no-empty-pattern -- vitest reads a hook's fixtures from this pattern.
  afterAll(({}, suite) => {
    expect(setUps).toBe(1);
    expect(outcomes(suite.tasks).map((outcome) => outcome.state)).toEqual([
      "pass",
      "pass",
      "pass",
    ]);
  });

  test("runs before the first test", () => {
    expect(setUps).toBe(1);
  });
  test("and not again before the second", () => {
    expect(setUps).toBe(1);
  });
  test("nor the third", () => {
    expect(setUps).toBe(1);
  });
});

describe("a setup whose start stalls, on CI", () => {
  let setUps = 0;
  const bodies: string[] = [];
  beforeApp(async () => {
    setUps++;
    setCiForTests(true);
    setFrameBudgetForTests(300);
    framesStop();
    await startApp();
  });

  // eslint-disable-next-line no-empty-pattern -- vitest reads a hook's fixtures from this pattern.
  afterAll(({}, suite) => {
    restore();
    expect(setUps).toBe(1);
    expect(bodies).toEqual([]);
    for (const outcome of outcomes(suite.tasks)) {
      expect(outcome.state).toBe("skip");
      expect(outcome.note).toMatch(/its rendering stalled/);
    }
    expect(suite.tasks).toHaveLength(3);
  });

  test("skips the first test", () => {
    bodies.push("first");
  });
  test("and the second", () => {
    bodies.push("second");
  });
  test("and the third, without starting again", () => {
    bodies.push("third");
  });
});

describe("a setup whose start stalls, off CI", () => {
  let setUps = 0;
  beforeApp(async () => {
    setUps++;
    setCiForTests(false);
    setFrameBudgetForTests(300);
    framesStop();
    await startApp();
  });

  afterAll(() => {
    restore();
    expect(setUps).toBe(1);
  });

  test.fails("fails the first test", () => undefined);
  test.fails("and the second", () => undefined);
});

describe("a setup that throws", () => {
  let setUps = 0;
  beforeApp(() => {
    setUps++;
    throw new Error("the setup failed");
  });

  afterAll(() => {
    expect(setUps).toBe(1);
  });

  test.fails("fails the first test", () => undefined);
  test.fails("and the second, without setting up again", () => undefined);
});

describe("a later block", () => {
  beforeApp(async () => {
    await startApp();
  });

  test("starts as before, once the stalled blocks restored the frames", () => {
    expect(window.orizonDebug?.frameCount ?? 0).toBeGreaterThan(0);
  });
});
