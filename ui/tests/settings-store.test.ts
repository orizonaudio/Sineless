import { describe, expect, test } from "vitest";

import type { SnapshotMessage } from "../../protocol/src/messages.ts";
import { defaultSettingValues } from "../../protocol/src/setting-values.ts";
import { SettingsStore } from "../src/store/settings-store.ts";

function snapshot(): SnapshotMessage {
  return {
    type: "snapshot",
    protocolVersion: 1,
    settings: { ...defaultSettingValues(), triggerMode: "normal" },
    sampleRate: 48000,
    inputChannels: 2,
  };
}

describe("settings store", () => {
  test("starts with schema defaults and disconnected", () => {
    const store = new SettingsStore();
    expect(store.connected).toBe(false);
    expect(store.get("triggerMode")).toBe("auto");
    expect(store.number("timeSpan")).toBe(1);
  });

  test("takes every value from the snapshot", () => {
    const store = new SettingsStore();
    store.applySnapshot(snapshot());
    expect(store.connected).toBe(true);
    expect(store.get("triggerMode")).toBe("normal");
  });

  test("notifies subscribers of engine-reported values", () => {
    const store = new SettingsStore();
    const changes: string[] = [];
    const unsubscribe = store.subscribe((name, value) =>
      changes.push(`${name}=${String(value)}`),
    );
    store.applyState("timeSpan", 0.5);
    unsubscribe();
    store.applyState("timeSpan", 1);
    expect(changes).toEqual(["timeSpan=0.5"]);
    expect(store.number("timeSpan")).toBe(1);
  });
});
