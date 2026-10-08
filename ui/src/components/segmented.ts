// A row of mutually exclusive buttons. Shows only values set with show(); clicks only request.

export interface Choice {
  readonly value: string;
  readonly label: string;
  // Help topic of this choice; otherwise the container's applies.
  readonly help?: string;
  // A small tag after the label, such as "beta".
  readonly badge?: string;
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
      button.textContent = choice.label;
      if (choice.badge !== undefined) {
        const badge = document.createElement("span");
        badge.className = "badge";
        badge.textContent = choice.badge;
        button.append(badge);
        button.setAttribute("aria-label", `${choice.label} (${choice.badge})`);
      }
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
