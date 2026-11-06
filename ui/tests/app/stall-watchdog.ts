// A vitest reporter that fails a browser project of the app tests whose page stops making
// progress, saying where it stopped (stall-tracker.ts), instead of letting the run wait with no
// output until the CI job's time limit. Registered in vitest.config.ts; it watches browser
// projects only.
import { execFile } from "node:child_process";
import { relative } from "node:path";

import type {
  ReportedHookContext,
  Reporter,
  TestCase,
  TestModule,
  TestProject,
  TestSpecification,
} from "vitest/node";

import { respondToStall } from "./stall-response.ts";
import {
  stallLimitMs,
  StallTracker,
  type WatchedProject,
} from "./stall-tracker.ts";

// How often the projects are checked; a stall is noticed within this after its limit.
const checkMs = 10_000;

export interface WatchdogOptions {
  // ORIZON_APP_STALL_SECONDS: another limit, for a slow machine.
  readonly stallSeconds?: string | undefined;
  // A monotonic clock, in ms: a laptop's sleep during a local run is not a stall.
  readonly now?: () => number;
  // What to do about a stalled project.
  readonly respond?: (project: TestProject, report: string) => Promise<void>;
}

// Playwright's browser processes, on macOS and Linux (a page stuck in JavaScript shows a busy
// WebContent or renderer process); Windows has no ps, so the list is unavailable.
function listBrowsers(): Promise<string> {
  if (process.platform === "win32") {
    return Promise.reject(new Error("no ps"));
  }
  return new Promise((resolve, reject) => {
    execFile(
      "ps",
      ["-axo", "pid,ppid,stat,%cpu,etime,command"],
      { timeout: 5000 },
      (error, stdout) => {
        if (error) {
          reject(new Error(`ps failed: ${error.message}`));
          return;
        }
        const [header = "", ...lines] = stdout.split("\n");
        // Playwright's own browsers, not the user's: all run from its ms-playwright folder.
        const browsers = lines.filter((line) => line.includes("ms-playwright"));
        resolve([header, ...browsers].join("\n"));
      },
    );
  });
}

function respond(project: TestProject, report: string): Promise<void> {
  return respondToStall(report, {
    write: (text) => process.stderr.write(text),
    list: listBrowsers,
    close: async () => {
      await project.browser?.closeBrowserProvider();
    },
    exit: (code) => process.exit(code),
  });
}

export default class StallWatchdog implements Reporter {
  private readonly tracker = new StallTracker();
  private readonly projects = new Map<string, TestProject>();
  private timer: ReturnType<typeof setInterval> | undefined;
  private readonly stallSeconds: string | undefined;
  private readonly now: () => number;
  private readonly respond: (
    project: TestProject,
    report: string,
  ) => Promise<void>;

  constructor(options: WatchdogOptions = {}) {
    this.stallSeconds =
      "stallSeconds" in options
        ? options.stallSeconds
        : process.env["ORIZON_APP_STALL_SECONDS"];
    this.now = options.now ?? (() => performance.now());
    this.respond = options.respond ?? respond;
  }

  onTestRunStart(specifications: readonly TestSpecification[]): void {
    this.projects.clear();
    const watched = new Map<string, WatchedProject>();
    for (const { project } of specifications) {
      if (!project.config.browser.enabled) {
        continue;
      }
      const name = project.name;
      this.projects.set(name, project);
      watched.set(name, {
        files: (watched.get(name)?.files ?? 0) + 1,
        limitMs: stallLimitMs(this.stallSeconds, project.config.testTimeout),
      });
    }
    this.tracker.start(watched);
    clearInterval(this.timer);
    this.timer = setInterval(() => {
      this.check();
    }, checkMs);
    this.timer.unref();
  }

  onTestModuleQueued(module: TestModule): void {
    this.progress(module.project, `${file(module)} was queued`);
  }

  onTestModuleCollected(module: TestModule): void {
    this.progress(module.project, `${file(module)} was collected`);
  }

  onTestModuleStart(module: TestModule): void {
    this.progress(module.project, `${file(module)} started`);
  }

  onTestCaseReady(test: TestCase): void {
    this.progress(test.project, `test "${test.fullName}" started`);
  }

  onTestCaseResult(test: TestCase): void {
    this.progress(test.project, `test "${test.fullName}" ended`);
  }

  onHookStart(hook: ReportedHookContext): void {
    this.progress(hook.entity.project, `${hook.name} of ${name(hook)} started`);
  }

  onHookEnd(hook: ReportedHookContext): void {
    this.progress(hook.entity.project, `${hook.name} of ${name(hook)} ended`);
  }

  onTestModuleEnd(module: TestModule): void {
    this.tracker.fileEnded(
      module.project.name,
      `${file(module)} ended`,
      this.now(),
    );
  }

  onTestRunEnd(): void {
    clearInterval(this.timer);
    this.timer = undefined;
    this.tracker.stop();
  }

  // Checks the projects now; the timer calls it, and so can a test.
  check(): void {
    for (const stall of this.tracker.check(this.now())) {
      const project = this.projects.get(stall.project);
      if (project !== undefined) {
        void this.respond(project, stall.report);
      }
    }
  }

  private progress(project: TestProject, what: string): void {
    this.tracker.progress(project.name, what, this.now());
  }
}

function file(module: TestModule): string {
  return relative(process.cwd(), module.moduleId);
}

function name(hook: ReportedHookContext): string {
  return hook.entity.type === "module"
    ? file(hook.entity)
    : `"${hook.entity.fullName}"`;
}
