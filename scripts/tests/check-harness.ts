// Runs a check script (scripts/check.sh, scripts/check-linux.sh) for real, in a temporary copy of
// the repository's layout, with the tools it calls replaced by stubs. Each stub logs its call, and
// fails when the call matches the pattern a test gives (STUB_FAIL, an extended regular
// expression), with STUB_STATUS (default 1), so a test can fail any stage and see which others
// still ran.
import { spawnSync } from "node:child_process";
import {
  chmodSync,
  copyFileSync,
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";

const repository = resolve(import.meta.dirname, "../..");

// What a stub prints, for the calls whose output a script reads.
const stubOutput = `
case "$name $*" in
  "git ls-files"*) printf '%s\\n' engine/src/A.cpp editor/src/mac/B.mm au/src/C.cpp ;;
  "uname -s") echo Linux ;;
  "nproc"*) echo 4 ;;
  "sysctl -n hw.ncpu") echo 4 ;;
  "brew --prefix llvm") echo "$STUB_LLVM" ;;
  "xcrun --show-sdk-path") echo /sdk ;;
  # Windows: robocopy's destination, as check.ps1's Bundle reads it, and the sanitizer runtime
  # the glue tests must load.
  "robocopy "*) mkdir -p "\${2//\\\\//}/Contents/x86_64-win" ;;
  "dumpbin "*) echo "    clang_rt.asan_dynamic-x86_64.dll" ;;
esac
`;

// A PowerShell script's stub is PowerShell, run by & as check.ps1 runs it; it fails by throwing,
// as the real one does.
const powerShellStub = `$name = [IO.Path]::GetFileName($PSCommandPath)
Add-Content $env:STUB_LOG "$name $args"
if ($env:STUB_FAIL -and "$name $args" -match $env:STUB_FAIL) { throw "$name failed" }
`;

const stub = (path: string) => {
  mkdirSync(dirname(path), { recursive: true });
  if (path.endsWith(".ps1")) {
    writeFileSync(path, powerShellStub);
    return;
  }
  writeFileSync(
    path,
    `#!/bin/bash
name="$(basename "$0")"
echo "$name $*" >> "$STUB_LOG"
${stubOutput}
if [[ -n "\${STUB_FAIL:-}" && "$name $*" =~ $STUB_FAIL ]]; then exit "\${STUB_STATUS:-1}"; fi
exit 0
`,
  );
  chmodSync(path, 0o755);
};

// Environment variables, whose upper-case names the naming rule would refuse as object keys.
export const variables = (...pairs: [string, string][]) =>
  Object.fromEntries(pairs);

export type Run = {
  status: number | null;
  output: string;
  // Every stubbed call, in order: the tool's name and its arguments.
  calls: string[];
};

export class Layout {
  readonly root = mkdtempSync(join(tmpdir(), "check-"));
  private readonly bin = join(this.root, ".stubs/bin");
  private readonly llvm = join(this.root, ".stubs/llvm");
  private readonly log = join(this.root, ".stubs/calls.log");

  // The real scripts, copied in; the tools each one calls, as stubs; and stubs for the scripts
  // given in `stubScripts` (paths from the repository's root).
  constructor(scripts: string[], tools: string[], stubScripts: string[] = []) {
    for (const script of scripts) {
      mkdirSync(join(this.root, dirname(script)), { recursive: true });
      copyFileSync(join(repository, script), join(this.root, script));
    }
    copyFileSync(
      join(repository, "package.json"),
      join(this.root, "package.json"),
    );
    for (const tool of tools) {
      stub(join(this.bin, tool));
    }
    for (const tool of ["clang-format", "clang-tidy", "clang++"]) {
      stub(join(this.llvm, tool));
    }
    for (const script of stubScripts) {
      stub(join(this.root, script));
    }
  }

  stub(path: string) {
    stub(join(this.root, path));
  }

  run(
    command: string[],
    env: Record<string, string> = {},
    shell = "bash",
  ): Run {
    rmSync(this.log, { force: true });
    const result = spawnSync(shell, command, {
      cwd: this.root,
      encoding: "utf8",
      env: {
        ...process.env,
        PATH: `${this.bin}:${process.env["PATH"] ?? ""}`,
        ...variables(
          ["LLVM_BIN", this.llvm],
          ["STUB_LLVM", dirname(this.llvm)],
          ["STUB_LOG", this.log],
        ),
        ...env,
      },
    });
    const calls = existsSync(this.log)
      ? readFileSync(this.log, "utf8").split("\n").filter(Boolean)
      : [];
    return {
      status: result.status,
      output: result.stdout + result.stderr,
      calls,
    };
  }

  remove() {
    rmSync(this.root, { recursive: true, force: true });
  }
}

// The stages a run reported in its summary, by name, with their results.
export const summary = (output: string) => {
  const start = output.lastIndexOf("==> Summary");
  const lines = start < 0 ? [] : output.slice(start).split("\n").slice(1);
  const stages = new Map<string, string>();
  for (const line of lines) {
    const match =
      /^ {2}(passed|FAILED|skipped|not run) +(?:[\d.]+ min)? +(.+?)(?: \(([^)]*)\))?$/.exec(
        line,
      );
    if (match) {
      stages.set(
        match[2] ?? "",
        `${match[1] ?? ""}${match[3] ? ` (${match[3]})` : ""}`,
      );
    }
  }
  return stages;
};
