// In Beats the grid stays as bright right of the playhead as left of it, in every grid type and
// display: the trace canvas over it draws nothing there but the traces (FR-9.2). Read along a row
// just under the screen's top, above the mock's traces and crossed by Grid's time lines, from the
// trace canvas straight after each draw, before the page composites it and its buffer is cleared.
import { afterEach, describe, expect, test, vi } from "vitest";

import { GridType } from "../../../protocol/src/enums.ts";
import type { GridTypeName } from "../../src/view/grid-settings.ts";
import { TraceRenderer } from "../../src/render/trace-renderer.ts";
import {
  byId,
  inTimeBase,
  settle,
  showDisplay,
  startApp,
  type Display,
} from "./harness.ts";

const types = Object.keys(GridType) as GridTypeName[];
const shown: readonly Display[] = ["Single", "Multi"];

afterEach(() => {
  vi.restoreAllMocks();
});

// The top row of the trace canvas as each draw left it.
function watchTopRow(): Uint8Array[] {
  const rows: Uint8Array[] = [];
  const { draw } = TraceRenderer.prototype as {
    draw: (
      this: TraceRenderer,
      ...args: Parameters<TraceRenderer["draw"]>
    ) => void;
  };
  vi.spyOn(TraceRenderer.prototype, "draw").mockImplementation(function (
    this: TraceRenderer,
    ...args: Parameters<TraceRenderer["draw"]>
  ) {
    draw.apply(this, args);
    const canvas = byId("trace") as HTMLCanvasElement;
    const gl = canvas.getContext("webgl2");
    if (gl === null) {
      return;
    }
    const row = new Uint8Array(canvas.width * 4);
    const y = canvas.height - 1 - Math.round(window.devicePixelRatio);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.readPixels(0, y, canvas.width, 1, gl.RGBA, gl.UNSIGNED_BYTE, row);
    rows.push(row);
  });
  return rows;
}

describe.each(shown)("in %s", (display) => {
  test.each(types)(
    "with grid type %s, the trace canvas leaves the grid right of the playhead uncovered",
    async (type) => {
      const app = await startApp();
      await showDisplay(display);
      await inTimeBase("Beats");
      app.transport.send({ type: "setSetting", name: "gridType", value: type });
      await settle(4);
      const rows = watchTopRow();
      await settle(6);
      expect(rows.length).toBeGreaterThan(0);
      for (const row of rows) {
        // The playhead: the opaque run; everything else on this row is the grid's to show.
        const alphas = Array.from(
          { length: row.length / 4 },
          (_, x) => row[x * 4 + 3] ?? 0,
        );
        const line = alphas.flatMap((alpha, x) => (alpha === 255 ? [x] : []));
        expect(line.length).toBeGreaterThan(0);
        const covered = alphas.filter(
          (alpha, x) =>
            alpha > 0 &&
            (x < Math.min(...line) - 1 || x > Math.max(...line) + 1),
        );
        expect(covered).toEqual([]);
      }
    },
  );
});
