// Help tooltips. While enabled, resting the pointer on an element with a data-help topic shows its
// explanation after a short delay; moving, pressing or scrolling hides it again, and none shows
// during a drag. The help switch
// explains itself even while help is off, so it can be found.
import { helpTexts, isHelpTopic } from "../view/help.ts";

const showDelay = 400; // ms
const offset = 14; // px between the pointer and the tooltip
const edge = 6; // px kept free at the window edges
const alwaysShown = new Set<string>(["help"]);

export class HelpTips {
  private readonly tip: HTMLElement;
  private enabled = false;
  private timer: number | undefined;

  constructor(tip: HTMLElement) {
    this.tip = tip;
    const hide = () => {
      this.hide();
    };
    document.addEventListener("pointermove", (event) => {
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

  setEnabled(enabled: boolean): void {
    this.enabled = enabled;
    this.hide();
  }

  private hide(): void {
    window.clearTimeout(this.timer);
    this.timer = undefined;
    this.tip.hidden = true;
  }

  private show(target: HTMLElement, x: number, y: number): void {
    const topic = target.dataset["help"] ?? "";
    if (!isHelpTopic(topic)) {
      return;
    }
    const { title, text } = helpTexts[topic];
    const heading = document.createElement("strong");
    heading.textContent = title;
    const body = document.createElement("span");
    body.textContent = text;
    this.tip.replaceChildren(heading, body);
    this.tip.hidden = false;
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
