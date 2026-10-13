import { defineConfig } from "vitest/config";

import { unitTests } from "./vitest.unit.ts";

// Mutation testing (tools/mutation) runs the unit tests alone, with the Vitest installed there:
// the app tests drive real browsers, far too slowly to run once per mutant.
export default defineConfig({ test: unitTests });
