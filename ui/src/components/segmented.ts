// A row of mutually exclusive buttons. Shows only values set with show(); clicks only request.

export interface Choice {
  readonly value: string;
  readonly label: string;
  // Shown instead of the label in the narrowest window.
  readonly short?: string;
  // Help topic of this choice; otherwise the container's applies.
  readonly help?: string;
}

export class Segmented {
  private readonly buttons = new Map<string, HTMLButtonElement>();

  constructor(
    container: HTMLElement,
    choices: readonly Choice[],
    onRequest: (value: string) => void,
  ) {
    for (const choice of choices) {
      const button = document.createElement("button");
      const long = document.createElement("span");
      long.className = "long";
      long.textContent = choice.label;
      const short = document.createElement("span");
      short.className = "short";
      short.textContent = choice.short ?? choice.label;
      button.append(long, short);
      button.setAttribute("aria-label", choice.label);
      if (choice.help !== undefined) {
        button.dataset["help"] = choice.help;
      }
      button.setAttribute("aria-pressed", "false");
      button.addEventListener("click", () => {
        onRequest(choice.value);
      });
      container.append(button);
      this.buttons.set(choice.value, button);
    }
  }

  show(value: string): void {
    for (const [choice, button] of this.buttons) {
      button.setAttribute("aria-pressed", String(choice === value));
    }
  }
}
