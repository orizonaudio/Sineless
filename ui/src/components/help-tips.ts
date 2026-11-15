// Help tooltips. While enabled, resting the pointer on an element with a data-help topic shows its
// explanation after a short delay; moving, pressing or scrolling hides it again, and none shows
// during a drag. The help switch
// explains itself even while help is off, so it can be found, and says at once that help is on
// when it turns it on: that tip stays by the switch until a press anywhere, or the pointer leaves
// the switch.
//
// A name cut to fit its place (cut-name.ts) rolls to show the rest; for a user who asked for
// reduced motion nothing rolls, and resting the pointer on the name, or moving the focus to it,
// shows the whole name in the tip instead, help on or off.
import { reducedMotion } from "./cut-name.ts";
import {
  type HelpText,
  type HelpTopic,
  helpTexts,
  isHelpTopic,
} from "../view/help.ts";

const showDelay = 400; // ms
export const offset = 14; // px between the pointer and the tooltip
export const edge = 6; // px kept free at the window edges
const alwaysShown = new Set<string>(["help"]);

export class HelpTips {
  private readonly tip: HTMLElement;
  private enabled = false;
  private timer: number | undefined;
  // The switch that turned help on, while its explanation shows.
  private pinnedTo: HTMLElement | undefined;

  constructor(tip: HTMLElement) {
    this.tip = tip;
    const hide = () => {
      this.hide();
    };
    document.addEventListener("pointermove", (event) => {
      // The hand moves a little after the click: neither hide the explanation nor let the
      // switch's own tip replace it.
      if (
        event.target instanceof Node &&
        this.pinnedTo?.contains(event.target) === true
      ) {
        return;
      }
      this.hide();
      // A move with a button held is a drag, not a rest.
      if (event.buttons === 0 && event.target instanceof Element) {
        const name = cutNameAt(event.target);
        if (name !== undefined) {
          const { clientX, clientY } = event;
          this.timer = window.setTimeout(() => {
            this.showName(name, clientX, clientY);
          }, showDelay);
          return;
        }
        const target = event.target.closest<HTMLElement>("[data-help]");
        if (
          target !== null &&
          (this.enabled || alwaysShown.has(target.dataset["help"] ?? ""))
        ) {
          const { clientX, clientY } = event;
          this.timer = window.setTimeout(() => {
            this.show(target, clientX, clientY);
          }, showDelay);
        }
      }
    });
    // The keyboard has no place to rest: the name shows at once, below what took the focus.
    document.addEventListener("focusin", (event) => {
      const name =
        event.target instanceof Element ? cutNameAt(event.target) : undefined;
      if (name !== undefined && event.target instanceof Element) {
        const box = event.target.getBoundingClientRect();
        this.showName(name, box.left, box.bottom);
      }
    });
    document.addEventListener("focusout", hide);
    document.addEventListener("pointerdown", hide);
    document.addEventListener("wheel", hide, { passive: true });
    document.documentElement.addEventListener("pointerleave", hide);
  }

  // Turned on from `from`, the help switch, it says so at once above the switch.
  setEnabled(enabled: boolean, from?: HTMLElement): void {
    this.enabled = enabled;
    this.hide();
    if (enabled && from !== undefined) {
      this.pin(from);
    }
  }

  private pin(button: HTMLElement): void {
    this.fill("helpOn");
    this.pinnedTo = button;
    button.addEventListener(
      "pointerleave",
      () => {
        if (this.pinnedTo === button) {
          this.hide();
        }
      },
      { once: true },
    );
    // Above the switch, right edges aligned, inside the window.
    const box = button.getBoundingClientRect();
    const { width, height } = this.tip.getBoundingClientRect();
    const left = Math.min(box.right - width, window.innerWidth - edge - width);
    this.tip.style.left = `${String(Math.max(edge, left))}px`;
    this.tip.style.top = `${String(Math.max(edge, box.top - offset / 2 - height))}px`;
  }

  private hide(): void {
    window.clearTimeout(this.timer);
    this.timer = undefined;
    this.pinnedTo = undefined;
    this.tip.hidden = true;
  }

  // Builds the tip from plain text, in reading order: title, lead, choices, actions, notes.
  private fill(topic: HelpTopic): void {
    const text: HelpText = helpTexts[topic];
    const part = (tag: string, className: string, content?: string) => {
      const element = document.createElement(tag);
      element.className = className;
      if (content !== undefined) {
        element.textContent = content;
      }
      return element;
    };
    const parts = [
      part("strong", "help-title", text.title),
      part("p", "help-lead", text.lead),
    ];
    if (text.choices !== undefined) {
      const list = part("dl", "help-choices");
      for (const { name, text: does } of text.choices) {
        const row = document.createElement("div");
        row.append(part("dt", "", name), part("dd", "", does));
        list.append(row);
      }
      parts.push(list);
    }
    if (text.actions !== undefined) {
      const list = part("dl", "help-actions");
      for (const { gesture, effect } of text.actions) {
        list.append(part("dt", "", gesture), part("dd", "", effect));
      }
      parts.push(list);
    }
    for (const note of text.notes ?? []) {
      parts.push(part("p", "help-note", note));
    }
    this.tip.replaceChildren(...parts);
    // Measured where it has the whole window, not squeezed against where the last tip stood.
    this.tip.style.left = "0";
    this.tip.style.top = "0";
    this.tip.hidden = false;
  }

  // The whole of a name that is cut where the pointer or the focus is.
  private showName(name: string, x: number, y: number): void {
    const text = document.createElement("p");
    text.className = "help-lead";
    text.textContent = name;
    this.tip.replaceChildren(text);
    this.tip.style.left = "0";
    this.tip.style.top = "0";
    this.tip.hidden = false;
    this.place(x, y);
  }

  private show(target: HTMLElement, x: number, y: number): void {
    const topic = target.dataset["help"] ?? "";
    if (!isHelpTopic(topic)) {
      return;
    }
    this.fill(topic);
    this.place(x, y);
  }

  private place(x: number, y: number): void {
    // Below and right of the pointer, flipped to the other side where the window ends.
    const { width, height } = this.tip.getBoundingClientRect();
    const right = x + offset + width <= window.innerWidth - edge;
    const below = y + offset + height <= window.innerHeight - edge;
    const left = right ? x + offset : x - offset - width;
    const top = below ? y + offset : y - offset - height;
    this.tip.style.left = `${String(Math.max(edge, left))}px`;
    this.tip.style.top = `${String(Math.max(edge, top))}px`;
  }
}

// The whole name of the cut name at `target`, or in the row or label it leads, where it shows in
// a tip: only for a user who asked for reduced motion (for any other it rolls).
function cutNameAt(target: Element): string | undefined {
  if (!reducedMotion()) {
    return undefined;
  }
  const holder = target.closest<HTMLElement>("[data-full-name], .names-cut");
  const name = holder?.matches("[data-full-name]")
    ? holder
    : holder?.querySelector<HTMLElement>("[data-full-name]");
  return name?.dataset["fullName"];
}
