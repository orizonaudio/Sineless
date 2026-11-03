// macOS native traces, the page's side: while the snapshot says the editor draws the traces, the
// page tells it what only the page knows (traceView), and shows what to do once native drawing has
// stopped for good. In WebGL mode (no nativeTraces) the page sends nothing new.
import { afterEach, describe, expect, test, vi } from "vitest";
import { page, userEvent } from "vitest/browser";

import { ViewLayout } from "../../../protocol/src/enums.ts";
import {
  defaultEditorWidth,
  minimumEditorWidth,
  screenBorder,
  screenMargin,
  statusBarHeight,
  toolbarHeight,
} from "../../../protocol/src/limits.ts";
import type { TraceViewMessage } from "../../../protocol/src/messages.ts";
import { graphicsMessages } from "../../src/app.ts";
import type { ControlMessage } from "../../src/bridge/transport.ts";
import { byId, choice, press, settle, startApp } from "./harness.ts";
import { zoomToNarrowest } from "./zoom-in.ts";

type Started = Awaited<ReturnType<typeof startApp>>;

const traceViews = (sent: readonly ControlMessage[]) =>
  sent.filter(
    (message) => message.type === "traceView",
  ) as unknown as TraceViewMessage[];

function snapshot(started: Started, extra: Record<string, unknown>): void {
  started.deliver({ ...started.transport.lastSnapshot(), ...extra });
}

// The editor says so in the first snapshot, before the page draws anything.
async function startNative(): Promise<Started> {
  const started = await startApp({ nativeTraces: true });
  await settle();
  return started;
}

const shownText = () =>
  byId("message").hidden ? undefined : byId("message-text").textContent;

afterEach(async () => {
  vi.restoreAllMocks();
  await page.viewport(1200, 720);
});

describe("native traces: the page under the editor's traces", () => {
  test("creates no WebGL context in native mode, as in WebGL mode it does", async () => {
    const getContext = vi.spyOn(HTMLCanvasElement.prototype, "getContext");
    await startNative();
    expect(
      getContext.mock.calls.filter(([kind]) => kind === "webgl2"),
    ).toHaveLength(0);
    getContext.mockClear();
    await startApp();
    await settle();
    expect(
      getContext.mock.calls.filter(([kind]) => kind === "webgl2"),
    ).not.toHaveLength(0);
  });

  // The editor's traces show through the page; the canvases stay laid out and take the pointer,
  // so hover, cursors, the trigger marker and drag-to-zoom work as before.
  test("is transparent over the screen and keeps its canvases laid out and hit-testable", async () => {
    await startNative();
    const transparent = "rgba(0, 0, 0, 0)";
    for (const element of [
      document.documentElement,
      document.body,
      byId("screen"),
    ]) {
      expect(getComputedStyle(element).backgroundColor).toBe(transparent);
    }
    for (const id of ["trace", "grid"]) {
      const canvas = byId(id);
      expect(getComputedStyle(canvas).opacity).toBe("0");
      expect(getComputedStyle(canvas).visibility).toBe("visible");
      expect(canvas.getBoundingClientRect().width).toBeGreaterThan(0);
    }
    const box = byId("trace").getBoundingClientRect();
    const hit = document.elementFromPoint(
      box.left + box.width / 2,
      box.top + box.height / 2,
    );
    expect(byId("screen").contains(hit)).toBe(true);
  });

  test("shows the hover readout over the editor's traces", async () => {
    await startNative();
    await press(byId("hover-toggle"));
    await userEvent.hover(byId("screen"), { position: { x: 300, y: 80 } });
    await expect.poll(() => byId("hover-readout").hidden).toBe(false);
    expect(byId("hover-readout").textContent).not.toBe("");
  });

  // The editor works the screen's rectangle out from the window's size and the insets in
  // limits.ts (editor/src/TraceView.cpp, ScreenRect); the page lays its screen out from the same
  // numbers, so the traces drawn natively sit exactly in it.
  test.each([
    { width: minimumEditorWidth, height: 420 },
    { width: defaultEditorWidth, height: 580 },
    { width: 1500, height: 900 },
    // Narrower than the minimum: the page keeps its minimum width and is clipped.
    { width: 900, height: 500 },
  ])(
    "lays the screen out where the editor draws, at $width × $height",
    async ({ width, height }) => {
      await page.viewport(width, height);
      await startNative();
      await settle();
      const inset = screenMargin + screenBorder;
      const box = byId("trace").getBoundingClientRect();
      expect(box.left).toBe(inset);
      expect(box.top).toBe(toolbarHeight + screenBorder);
      expect(box.width).toBe(Math.max(width, minimumEditorWidth) - 2 * inset);
      expect(box.height).toBe(
        height - toolbarHeight - statusBarHeight - 2 * screenBorder,
      );
    },
  );
});

describe("native traces: what the page sends", () => {
  test("in WebGL mode the page sends no traceView", async () => {
    const started = await startApp();
    await settle(6);
    expect(traceViews(started.sent)).toHaveLength(0);
  });

  test("once native, it sends the page's view, layout, scale and colours", async () => {
    const started = await startNative();
    const view = traceViews(started.sent).at(-1);
    expect(view).toBeDefined();
    expect(view?.viewStart).toBe(0);
    expect(view?.viewEnd).toBe(1);
    expect(Object.keys(ViewLayout)).toContain(view?.layout);
    expect(view?.lineWidth).toBeCloseTo(1.5 * window.devicePixelRatio);
    expect(view?.viewColours).toHaveLength(4);
    for (const colour of [
      ...(view?.viewColours ?? []),
      view?.screen,
      view?.grid,
      view?.axis,
      view?.cursor,
      view?.previous,
      view?.cancellation,
      view?.sumColour,
    ]) {
      expect(colour).toHaveLength(4);
      for (const channel of colour ?? []) {
        expect(channel).toBeGreaterThanOrEqual(0);
        expect(channel).toBeLessThanOrEqual(1);
      }
    }
  });

  test("frames alone send nothing new", async () => {
    const started = await startNative();
    const before = traceViews(started.sent).length;
    await settle(10);
    expect(traceViews(started.sent)).toHaveLength(before);
  });

  // Each field that changes while native sends a new traceView describing it.
  test.each([
    {
      field: "the view window, on a zoom",
      act: async () => {
        await zoomToNarrowest();
      },
      check: (view: TraceViewMessage) => {
        expect(view.viewEnd - view.viewStart).toBeLessThan(1);
      },
    },
    {
      field: "the layout",
      act: async () => {
        const overlaid =
          choice("layout", "Overlay").getAttribute("aria-pressed") === "true";
        await press(choice("layout", overlaid ? "Stack" : "Overlay"));
      },
      check: (view: TraceViewMessage, before: TraceViewMessage) => {
        expect(view.layout).not.toBe(before.layout);
      },
    },
    {
      field: "a new lane set",
      act: async (started: Started) => {
        started.deliver({
          type: "lanes",
          laneSet: 41,
          lanes: [
            {
              id: 9,
              name: "Kick",
              colour: 0x336699,
              own: true,
              compared: false,
            },
          ],
          hidden: 0,
        });
        await settle();
      },
      check: (view: TraceViewMessage) => {
        const set = view.laneSets.find((entry) => entry.laneSet === 41);
        expect(set?.lanes.map((lane) => lane.id)).toEqual([9]);
        expect(set?.lanes[0]?.colour).toEqual([
          0x33 / 255,
          0x66 / 255,
          0x99 / 255,
          1,
        ]);
      },
    },
  ])("$field sends a new traceView", async ({ act, check }) => {
    const started = await startNative();
    const before = traceViews(started.sent);
    await act(started);
    await settle();
    const sent = traceViews(started.sent);
    expect(sent.length).toBeGreaterThan(before.length);
    const last = sent.at(-1);
    const first = before.at(-1);
    expect(last).toBeDefined();
    expect(first).toBeDefined();
    if (last !== undefined && first !== undefined) {
      check(last, first);
    }
  });
});

describe("native traces: native graphics lost", () => {
  test("a snapshot saying so shows what to do", async () => {
    const started = await startApp();
    snapshot(started, { nativeTraces: true, nativeGraphicsLost: true });
    await settle();
    expect(shownText()).toBe(graphicsMessages.lost);
  });

  test("the message saying so shows what to do, and nothing clears it", async () => {
    const started = await startNative();
    expect(shownText()).toBeUndefined();
    started.deliver({ type: "nativeGraphicsLost" });
    await settle();
    expect(shownText()).toBe(graphicsMessages.lost);
    snapshot(started, { nativeTraces: true, nativeGraphicsLost: false });
    await settle();
    expect(shownText()).toBe(graphicsMessages.lost);
  });

  test("an engine error outranks it, and the lost text returns when the error clears", async () => {
    const started = await startNative();
    started.deliver({ type: "nativeGraphicsLost" });
    started.deliver({ type: "error", code: "protocolMismatch", message: "" });
    await settle();
    expect(shownText()).not.toBe(graphicsMessages.lost);
    snapshot(started, { nativeTraces: true });
    await settle();
    expect(shownText()).toBe(graphicsMessages.lost);
  });

  test("a snapshot without the field leaves the page's own messages alone", async () => {
    const started = await startApp();
    await settle();
    expect(shownText()).toBeUndefined();
    snapshot(started, {});
    await settle();
    expect(shownText()).toBeUndefined();
  });
});
