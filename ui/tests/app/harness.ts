// Starts the real page, as the plugin's web view loads it, on the mock back end. Each test file
// runs in a page of its own and starts the app once: the app finds its elements by id, so a
// second one in the same page would draw into the first one's.
import { expect, vi } from "vitest";
import { userEvent } from "vitest/browser";

import html from "../../index.html?raw";
import {
  MockTransport,
  type MockOptions,
} from "../../src/bridge/mock-transport.ts";
import type { ControlMessage, Transport } from "../../src/bridge/transport.ts";
import { startSineless } from "../../src/start.ts";
import "../../src/styles.css";

export interface Started {
  readonly transport: MockTransport;
  // Messages the page sent, in order.
  readonly sent: ControlMessage[];
  // Holds the page's request for a frame, as the engine does while nothing triggers, and
  // answers it when resumed.
  holdFrames(held: boolean): void;
  // Delivers a message to the page as if the engine had sent it.
  deliver(message: ControlMessage): void;
}

// Starts the page on a new mock back end, or on `transport`: the page reopened on the same
// instance, as when the editor window is closed and opened again.
export async function startApp(
  options: MockOptions = {},
  transport = new MockTransport(options),
): Promise<Started> {
  document.body.innerHTML = new DOMParser().parseFromString(
    html,
    "text/html",
  ).body.innerHTML;
  const sent: ControlMessage[] = [];
  let held = false;
  let waiting = false; // A request held back.
  let deliver: (message: ControlMessage) => void = () => undefined;
  const watched: Transport = {
    onMessage(handler) {
      deliver = handler;
      transport.onMessage(handler);
    },
    onFrame(handler) {
      transport.onFrame(handler);
    },
    send(message) {
      sent.push(message);
      transport.send(message);
    },
    requestFrame() {
      if (held) {
        waiting = true;
      } else {
        transport.requestFrame();
      }
    },
  };
  startSineless(watched);
  await vi.waitFor(
    () => {
      expect(window.orizonDebug?.frameCount ?? 0).toBeGreaterThan(0);
    },
    { timeout: 5000 },
  );
  return {
    transport,
    sent,
    holdFrames(value) {
      held = value;
      if (!held && waiting) {
        waiting = false;
        transport.requestFrame();
      }
    },
    deliver(message) {
      deliver(message);
    },
  };
}

// The element with this id; fails the test when the page has none.
export function byId(id: string): HTMLElement {
  const found = document.getElementById(id);
  expect(found, `#${id}`).not.toBeNull();
  return found as HTMLElement;
}

// A button of a segmented control, by its label.
export function choice(groupId: string, label: string): HTMLButtonElement {
  const button = [...byId(groupId).querySelectorAll("button")].find(
    (candidate) => candidate.firstChild?.textContent === label,
  );
  expect(button, `${groupId} › ${label}`).toBeDefined();
  return button as HTMLButtonElement;
}

// Clicks a control, first opening the toolbar popover it sits in (Group, Views, Trigger) if that
// is closed, as a user would.
export async function press(button: HTMLElement): Promise<void> {
  const popover = button.closest<HTMLElement>(".popover");
  if (popover?.hidden === true) {
    await userEvent.click(byId(popover.id.replace(/-popover$/, "-details")));
  }
  await userEvent.click(button);
}

// Waits until the page has drawn frames that reflect everything asked so far.
export async function settle(frames = 3): Promise<void> {
  const from = window.orizonDebug?.frameCount ?? 0;
  await vi.waitFor(
    () => {
      expect(window.orizonDebug?.frameCount ?? 0).toBeGreaterThanOrEqual(
        from + frames,
      );
    },
    { timeout: 5000 },
  );
}

// Types `text` into a number field, as a double-click and Enter do.
export async function typeInto(hostId: string, text: string): Promise<void> {
  const field = byId(hostId).querySelector<HTMLElement>(".field");
  expect(field, `${hostId} field`).not.toBeNull();
  await userEvent.dblClick(field as HTMLElement);
  const input = (field as HTMLElement).querySelector("input");
  expect(input, `${hostId} input`).not.toBeNull();
  await userEvent.fill(input as HTMLInputElement, text);
  await userEvent.keyboard("{Enter}");
}

// The status chips' texts, in order, those folded into "+n" included.
export function chips(): string[] {
  return [...byId("chips").querySelectorAll(".chip")].map(
    (chip) => chip.textContent,
  );
}

// The status chips shown in the status bar itself, in order: none folded into "+n".
export function shownChips(): string[] {
  return [...byId("chips").children]
    .filter((child) => child.classList.contains("chip"))
    .map((chip) => chip.textContent);
}

// A horizontal swipe over the traces of `screenWidths` (negative: back in time).
export function swipe(screenWidths: number): void {
  const screen = byId("screen");
  byId("trace").dispatchEvent(
    new WheelEvent("wheel", {
      deltaX: screenWidths * screen.clientWidth,
      deltaMode: WheelEvent.DOM_DELTA_PIXEL,
      bubbles: true,
      cancelable: true,
    }),
  );
}
