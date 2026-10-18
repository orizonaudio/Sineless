/* global process, setTimeout */
// Stands in for tests that cannot run beside another copy of themselves, as the Audio Unit's view
// tests share the window server: each run holds the lock file named by its argument for a moment,
// and fails if another run holds it.
import { rmSync, writeFileSync } from "node:fs";

const lock = process.argv[2] ?? "";
try {
  writeFileSync(lock, "", { flag: "wx" });
} catch {
  process.exit(1);
}
setTimeout(() => {
  rmSync(lock);
  process.exit(0);
}, 300);
