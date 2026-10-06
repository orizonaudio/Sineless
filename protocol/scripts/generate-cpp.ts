// Writes the generated C++ protocol header. Usage: node protocol/scripts/generate-cpp.ts <file>
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname } from "node:path";

import { generateCppHeader } from "../src/cpp-generator.ts";

const outputFile = process.argv[2];
if (outputFile === undefined) {
  console.error("Usage: generate-cpp.ts <output-file>");
  process.exit(1);
}

const header = generateCppHeader();
let existing: string | undefined;
try {
  existing = readFileSync(outputFile, "utf8");
} catch {
  existing = undefined;
}
// Leave an unchanged header untouched so dependent C++ is not rebuilt.
if (existing !== header) {
  mkdirSync(dirname(outputFile), { recursive: true });
  writeFileSync(outputFile, header);
}
