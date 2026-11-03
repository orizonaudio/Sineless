import { expect, test } from "vitest";

import {
  chromeHeight,
  screenBorder,
  statusBarHeight,
  toolbarHeight,
} from "../src/limits.ts";

// The minimum heights reserve chromeHeight for everything around the screen; it must hold the
// toolbar, the status bar and the screen's border, or a stacked lane would get less than its
// minimum.
test("the chrome height holds the toolbar, status bar and the screen's border", () => {
  expect(chromeHeight).toBeGreaterThanOrEqual(
    toolbarHeight + statusBarHeight + 2 * screenBorder,
  );
});
