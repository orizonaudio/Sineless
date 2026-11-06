// Help tooltips. While enabled, resting the pointer on an element with a data-help topic shows its
// explanation after a short delay; moving, pressing or scrolling hides it again, and none shows
// during a drag. The help switch
// explains itself even while help is off, so it can be found, and says at once that help is on
// when it turns it on: that tip stays by the switch until a press anywhere, or the pointer leaves
// the switch.
import { type HelpTopic, helpTexts, isHelpTopic } from "../view/help.ts";

const showDelay = 400; // ms
const offset = 14; // px between the pointer and the tooltip
const edge = 6; // px kept free at the window edges
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

  private fill(topic: HelpTopic): void {
    const { title, text } = helpTexts[topic];
    const heading = document.createElement("strong");
    heading.textContent = title;
    const body = document.createElement("span");
    body.textContent = text;
    this.tip.replaceChildren(heading, body);
    this.tip.hidden = false;
  }

  private show(target: HTMLElement, x: number, y: number): void {
    const topic = target.dataset["help"] ?? "";
    if (!isHelpTopic(topic)) {
      return;
    }
    this.fill(topic);
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
