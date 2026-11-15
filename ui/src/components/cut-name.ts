// A name shown in a width it may not fit: cut in the middle at rest (middle-cut.ts), and, where
// it is live, rolled slowly to its end and back while the pointer or the keyboard focus is on it,
// so the whole name can be read. To a screen reader it is always the whole name.
//
// The roll starts from an indirect interaction, so it is one pass that ends by itself within five
// seconds (WCAG 2.2.2) and stops when the pointer or the focus leaves. A user who asked for
// reduced motion gets no roll (WCAG 2.3.3, prefers-reduced-motion): the whole name shows in a tip
// instead (help-tips.ts reads it from data-full-name), as macOS shows a clipped text whole in an
// expansion tooltip.
import { middleCut } from "../view/middle-cut.ts";

// How fast a name rolls, in pixels a second: slow enough to read as it passes.
const rollSpeed = 30;
// How long a rolled name rests at its start before it moves, and at its end before it returns.
const rollRestSeconds = 0.5;
// The longest a roll lasts, rests included (WCAG 2.2.2 allows motion started by hover or focus
// up to five seconds without a way to pause it); a longer name rolls faster to keep within it.
const rollMaxSeconds = 5;

const fitters = new WeakMap<Element, () => void>();

export interface CutNameOptions {
  // Whether the name stays on screen: it then rolls under the pointer and the focus, and is fitted
  // again when the fonts have loaded. Every name is fitted when fitCutNames is asked to, once it
  // is in the page and laid out (its place's width is the page's to know: a name's own observer
  // would outlive the page that made it).
  readonly live: boolean;
  // The element whose hover and focus roll the name: the name itself unless given (a list row,
  // whose check box takes the focus).
  readonly trigger?: HTMLElement;
}

export function reducedMotion(): boolean {
  return window.matchMedia("(prefers-reduced-motion: reduce)").matches;
}

// The element showing `full`, cut to the width its place gives it.
export function cutName(full: string, options: CutNameOptions): HTMLElement {
  const outer = document.createElement("span");
  outer.className = "cut-name";
  const shown = document.createElement("span");
  shown.className = "cut-shown";
  shown.textContent = full;
  const said = document.createElement("span");
  said.className = "visually-hidden";
  said.textContent = full;
  outer.append(shown);

  let roll: Animation | undefined;
  const fit = () => {
    if (roll !== undefined) {
      return;
    }
    shown.textContent = full;
    const room = outer.clientWidth;
    if (room === 0) {
      // Not laid out (in a closed popover): fitted when it is.
      return;
    }
    const text = middleCut(full, (candidate) => {
      shown.textContent = candidate;
      return outer.scrollWidth <= outer.clientWidth;
    });
    shown.textContent = text;
    const cut = text !== full;
    // Cut, the piece shown is not the name: the whole name is what a screen reader says, and
    // what the tip shows where nothing rolls.
    outer.classList.toggle("cut", cut);
    if (cut) {
      shown.setAttribute("aria-hidden", "true");
      outer.dataset["fullName"] = full;
      outer.append(said);
    } else {
      shown.removeAttribute("aria-hidden");
      delete outer.dataset["fullName"];
      said.remove();
    }
  };
  fitters.set(outer, fit);
  if (!options.live) {
    return outer;
  }

  // The fonts decide what fits: fitted again once they have loaded.
  void document.fonts.ready.then(fit);

  const stop = () => {
    if (roll === undefined) {
      return;
    }
    const ended = roll;
    roll = undefined;
    ended.cancel();
    outer.classList.remove("rolling");
    outer.style.width = "";
    fit();
  };
  const start = () => {
    if (roll !== undefined || !outer.classList.contains("cut")) {
      return;
    }
    if (reducedMotion()) {
      return;
    }
    // The name keeps the width it has cut, so nothing beside it moves while it rolls.
    outer.style.width = `${String(outer.getBoundingClientRect().width)}px`;
    outer.classList.add("rolling");
    shown.textContent = full;
    const over = outer.scrollWidth - outer.clientWidth;
    const travel = Math.min(
      over / rollSpeed,
      (rollMaxSeconds - 2 * rollRestSeconds) / 2,
    );
    const total = 2 * (travel + rollRestSeconds);
    const away = `translateX(${String(-over)}px)`;
    roll = shown.animate(
      [
        { transform: "translateX(0)", offset: 0 },
        { transform: "translateX(0)", offset: rollRestSeconds / total },
        { transform: away, offset: (rollRestSeconds + travel) / total },
        { transform: away, offset: (2 * rollRestSeconds + travel) / total },
        { transform: "translateX(0)", offset: 1 },
      ],
      { duration: total * 1000, easing: "ease-in-out" },
    );
    roll.addEventListener("finish", stop);
  };
  const trigger = options.trigger ?? outer;
  trigger.addEventListener("pointerenter", start);
  trigger.addEventListener("pointerleave", stop);
  trigger.addEventListener("focusin", start);
  trigger.addEventListener("focusout", stop);
  return outer;
}

// Fits the names under `root`, once they are in the page and laid out: when they are built, and
// when what holds them comes on show.
export function fitCutNames(root: ParentNode): void {
  for (const name of root.querySelectorAll(".cut-name")) {
    fitters.get(name)?.();
  }
}
