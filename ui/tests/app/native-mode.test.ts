// macOS native traces, the page's side: while the snapshot says the editor draws the traces, the
// page tells it what only the page knows (traceView), and shows what to do once native drawing has
// stopped for good. In WebGL mode (no nativeTraces) the page sends nothing new.
import { afterEach, describe, expect, test, vi } from "vitest";

import { ViewLayout } from "../../../protocol/src/enums.ts";
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

async function startNative(): Promise<Started> {
  const started = await startApp();
  snapshot(started, { nativeTraces: true, nativeGraphicsLost: false });
  await settle();
  return started;
}

const shownText = () =>
  byId("message").hidden ? undefined : byId("message-text").textContent;

afterEach(() => {
  vi.restoreAllMocks();
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
