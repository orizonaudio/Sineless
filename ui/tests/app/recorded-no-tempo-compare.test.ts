// See no-tempo-recorded.ts.
import { describe } from "vitest";

import { noTempoTests } from "./no-tempo-recorded.ts";

describe("a recorded Compare view with no tempo", () => {
  noTempoTests("NoTempo_Compare_48k");
});
