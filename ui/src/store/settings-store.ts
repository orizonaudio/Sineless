// The UI's mirror of the engine's settings (DESIGN 5.1). C++ is the store of record: the store
// changes only when the engine reports a value (snapshot or stateChanged), never when the UI asks
// for one, so it cannot drift from the engine.
import type {
  SettingValue,
  SnapshotMessage,
} from "../../../protocol/src/messages.ts";
import { defaultSettingValues } from "../../../protocol/src/setting-values.ts";
import type { SettingName } from "../../../protocol/src/settings.ts";

export type SettingsListener = (name: SettingName, value: SettingValue) => void;

export class SettingsStore {
  private readonly values: Record<SettingName, SettingValue> =
    defaultSettingValues();
  private readonly listeners = new Set<SettingsListener>();
  private connectedState = false;

  get connected(): boolean {
    return this.connectedState;
  }

  get(name: SettingName): SettingValue {
    return this.values[name];
  }

  number(name: SettingName): number {
    const value = this.values[name];
    return typeof value === "number" ? value : Number.NaN;
  }

  // Returns a function that removes the listener.
  subscribe(listener: SettingsListener): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  applySnapshot(snapshot: SnapshotMessage): void {
    this.connectedState = true;
    for (const [name, value] of Object.entries(snapshot.settings) as [
      SettingName,
      SettingValue,
    ][]) {
      this.applyState(name, value);
    }
  }

  applyState(name: SettingName, value: SettingValue): void {
    if (!(name in this.values)) {
      return;
    }
    this.values[name] = value;
    for (const listener of this.listeners) {
      listener(name, value);
    }
  }
}
