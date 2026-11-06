import { afterEach, beforeEach, describe, expect, test, vi } from "vitest";
import type {
  ReportedHookContext,
  TestCase,
  TestModule,
  TestProject,
  TestSpecification,
} from "vitest/node";

import StallWatchdog, { playwrightProcesses } from "./app/stall-watchdog.ts";

// Stand-ins for vitest's objects, with only what the watchdog reads.
function project(
  name: string,
  browser: boolean,
  testTimeout = 30_000,
): TestProject {
  return {
    name,
    config: { browser: { enabled: browser }, testTimeout },
  } as unknown as TestProject;
}

function spec(owner: TestProject): TestSpecification {
  return { project: owner } as unknown as TestSpecification;
}

function module(owner: TestProject, file: string): TestModule {
  return {
    project: owner,
    moduleId: `${process.cwd()}/ui/tests/app/${file}`,
  } as unknown as TestModule;
}

function testCase(owner: TestProject, name: string): TestCase {
  return { project: owner, fullName: name } as unknown as TestCase;
}

// The watchdog follows each browser project's page through vitest's events and acts on the one
// that went silent: the run then fails at once, naming where, instead of waiting for the job's
// time limit.
describe("the stall watchdog", () => {
  let now = 0;
  let stalled: { project: string; report: string }[];

  beforeEach(() => {
    vi.useFakeTimers();
    now = 0;
    stalled = [];
  });
  afterEach(() => {
    vi.useRealTimers();
  });

  function watchdog(stallSeconds?: string): StallWatchdog {
    return new StallWatchdog({
      stallSeconds,
      now: () => now,
      respond: (stalledProject, report) => {
        stalled.push({ project: stalledProject.name, report });
        return Promise.resolve();
      },
    });
  }

  test("acts on the silent browser project only, with its last event, after four test timeouts", () => {
    const webkit = project("app (webkit)", true);
    const chromium = project("app (chromium)", true);
    const unit = project("unit", false);
    const dog = watchdog();
    dog.onTestRunStart([
      spec(webkit),
      spec(webkit),
      spec(chromium),
      spec(unit),
    ]);
    dog.onTestModuleQueued(module(webkit, "page-barcode.test.ts"));
    dog.onTestModuleQueued(module(chromium, "a.test.ts"));
    dog.onTestModuleQueued(module(unit, "x.test.ts"));
    now = 100_000;
    dog.onTestCaseReady(testCase(chromium, "a"));
    now = 119_999;
    dog.check();
    expect(stalled).toEqual([]);
    now = 120_000;
    dog.check();
    expect(stalled.map((stall) => stall.project)).toEqual(["app (webkit)"]);
    expect(stalled[0]?.report).toContain(
      "since ui/tests/app/page-barcode.test.ts was queued",
    );
  });

  test("follows each project's own test timeout", () => {
    const webkit = project("app (webkit)", true, 10_000);
    const dog = watchdog();
    dog.onTestRunStart([spec(webkit)]);
    dog.onTestModuleStart(module(webkit, "a.test.ts"));
    now = 40_000;
    dog.check();
    expect(stalled.map((stall) => stall.project)).toEqual(["app (webkit)"]);
  });

  test("checks by itself while the run goes on, and stops when it ends", async () => {
    const webkit = project("app (webkit)", true);
    const dog = watchdog("20");
    dog.onTestRunStart([spec(webkit), spec(webkit)]);
    dog.onTestModuleEnd(module(webkit, "a.test.ts"));
    now = 30_000;
    await vi.advanceTimersByTimeAsync(10_000);
    expect(stalled).toHaveLength(1);
    dog.onTestRunEnd();
    expect(vi.getTimerCount()).toBe(0);
  });

  test("a project whose files have all ended is not stalled", async () => {
    const webkit = project("app (webkit)", true);
    const dog = watchdog("20");
    dog.onTestRunStart([spec(webkit)]);
    dog.onTestModuleQueued(module(webkit, "a.test.ts"));
    dog.onTestModuleEnd(module(webkit, "a.test.ts"));
    now = 600_000;
    await vi.advanceTimersByTimeAsync(10_000);
    expect(stalled).toEqual([]);
  });

  test("watches a later run of the same process (watch mode) as well", () => {
    const webkit = project("app (webkit)", true);
    const dog = watchdog("20");
    dog.onTestRunStart([spec(webkit)]);
    dog.onTestRunEnd();
    dog.onTestRunStart([spec(webkit)]);
    dog.onTestModuleQueued(module(webkit, "a.test.ts"));
    now = 20_000;
    dog.check();
    expect(stalled).toHaveLength(1);
  });

  test.each([
    [
      "a file collected",
      (dog: StallWatchdog, owner: TestProject) => {
        dog.onTestModuleCollected(module(owner, "a.test.ts"));
      },
      "ui/tests/app/a.test.ts was collected",
    ],
    [
      "a test ended",
      (dog: StallWatchdog, owner: TestProject) => {
        dog.onTestCaseResult(testCase(owner, "draws"));
      },
      'test "draws" ended',
    ],
    [
      "a file's hook started",
      (dog: StallWatchdog, owner: TestProject) => {
        dog.onHookStart({
          name: "beforeAll",
          entity: Object.assign(module(owner, "a.test.ts"), { type: "module" }),
        } as unknown as ReportedHookContext);
      },
      "beforeAll of ui/tests/app/a.test.ts started",
    ],
    [
      "a test's hook ended",
      (dog: StallWatchdog, owner: TestProject) => {
        dog.onHookEnd({
          name: "beforeEach",
          entity: Object.assign(testCase(owner, "draws"), { type: "test" }),
        } as unknown as ReportedHookContext);
      },
      'beforeEach of "draws" ended',
    ],
  ])("counts %s as progress, and names it", (_, send, named) => {
    const webkit = project("app (webkit)", true);
    const dog = watchdog("20");
    dog.onTestRunStart([spec(webkit)]);
    dog.onTestModuleQueued(module(webkit, "a.test.ts"));
    now = 15_000;
    send(dog, webkit);
    now = 34_999;
    dog.check();
    expect(stalled).toEqual([]);
    now = 35_000;
    dog.check();
    expect(stalled[0]?.report).toContain(`since ${named}`);
  });
});

// The process list shows Playwright's browsers only: on a developer's machine, not every process
// running (their own Chrome among them).
test("the process list keeps the header and Playwright's browsers", () => {
  const ps = [
    "  PID  PPID STAT  %CPU     ELAPSED COMMAND",
    "  189 93839 S      0.0 04-12:56:28 /Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "37491 37485 SN     0.1       00:30 /Users/u/Library/Caches/ms-playwright/webkit-2370/Playwright.app/Contents/MacOS/Playwright",
    "37495     1 Rs    99.2       00:30 /Users/u/Library/Caches/ms-playwright/webkit-2370/com.apple.WebKit.WebContent.xpc/Contents/MacOS/com.apple.WebKit.WebContent.Development",
  ].join("\n");
  expect(playwrightProcesses(ps).split("\n")).toEqual([
    "  PID  PPID STAT  %CPU     ELAPSED COMMAND",
    "37491 37485 SN     0.1       00:30 /Users/u/Library/Caches/ms-playwright/webkit-2370/Playwright.app/Contents/MacOS/Playwright",
    "37495     1 Rs    99.2       00:30 /Users/u/Library/Caches/ms-playwright/webkit-2370/com.apple.WebKit.WebContent.xpc/Contents/MacOS/com.apple.WebKit.WebContent.Development",
  ]);
});
