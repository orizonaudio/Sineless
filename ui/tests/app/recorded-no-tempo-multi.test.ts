// See no-tempo-recorded.ts.
import { describe } from "vitest";

import { noTempoTests } from "./no-tempo-recorded.ts";

describe("a recorded Multi view with no tempo", () => {
  noTempoTests("NoTempo_Multi_48k");
});
