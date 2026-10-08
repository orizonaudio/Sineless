// Control messages exchanged as JSON over the native channel. Every message
// is an object whose `type` holds one of the MessageType values; the payloads are below.
// Setting values use the JSON form of their kind: numbers, enum value names, or arrays of names.
import type { SettingName } from "./settings.ts";

export const MessageType = {
  hello: "hello",
  snapshot: "snapshot",
  setSetting: "setSetting",
  stateChanged: "stateChanged",
  error: "error",
  viewport: "viewport",
  freeze: "freeze",
  pan: "pan",
  arm: "arm",
  view: "view",
  lanes: "lanes",
  compare: "compare",
  previewFix: "previewFix",
} as const;
export type MessageType = (typeof MessageType)[keyof typeof MessageType];

export const ErrorCode = {
  protocolMismatch: "protocolMismatch",
  // The editor cannot reach the processor's engine.
  engineUnavailable: "engineUnavailable",
} as const;
export type ErrorCode = (typeof ErrorCode)[keyof typeof ErrorCode];

export type SettingValue = number | string | readonly string[];

// UI to engine. Starts (or restarts) a session; `viewWidth` is the trace area in CSS pixels.
export interface HelloMessage {
  readonly type: "hello";
  readonly protocolVersion: number;
  readonly viewWidth: number;
  readonly pixelRatio: number;
}

// Engine to UI. The full state, in reply to hello.
export interface SnapshotMessage {
  readonly type: "snapshot";
  readonly protocolVersion: number;
  readonly settings: Readonly<Record<SettingName, SettingValue>>;
  readonly sampleRate: number;
  readonly inputChannels: number;
}

// UI to engine. The engine always answers with stateChanged holding the value it kept.
export interface SetSettingMessage {
  readonly type: "setSetting";
  readonly name: SettingName;
  readonly value: SettingValue;
}

// Engine to UI. A setting's current value, after setSetting or a host state change.
export interface StateChangedMessage {
  readonly type: "stateChanged";
  readonly name: SettingName;
  readonly value: SettingValue;
}

// Engine to UI.
export interface ErrorMessage {
  readonly type: "error";
  readonly code: ErrorCode;
  readonly message: string;
}

// UI to engine. The trace area was resized.
export interface ViewportMessage {
  readonly type: "viewport";
  readonly viewWidth: number;
  readonly pixelRatio: number;
}

// UI to engine. Freeze or resume the display.
export interface FreezeMessage {
  readonly type: "freeze";
  readonly frozen: boolean;
}

// UI to engine. While frozen, how far back to move the view, in seconds.
export interface PanMessage {
  readonly type: "pan";
  readonly seconds: number;
}

// UI to engine. Single mode: wait for the next trigger again.
export interface ArmMessage {
  readonly type: "arm";
}

// UI to engine. The part of the span on screen, as fractions; a temporary view, not a
// setting. The whole span is { start: 0, end: 1 }.
export interface ViewMessage {
  readonly type: "view";
  readonly start: number;
  readonly end: number;
}

// One track of the multi-track view. `id` is stable while the track exists; `name` is empty and
// `colour` null when the host reports none; `own` marks the instance whose window shows the lanes.
// In the Compare view, `compared` marks the tracks summed and measured against this one (this
// track included whenever another is).
export interface LaneLabel {
  readonly id: number;
  readonly name: string;
  readonly colour: number | null; // 0xRRGGBB
  readonly own: boolean;
  readonly compared: boolean;
}

// Engine to UI. The tracks behind the lanes of frames whose laneSet matches, in lane order, and
// how many more tracks of the group are not drawn. Sent when the set changes.
export interface LanesMessage {
  readonly type: "lanes";
  readonly laneSet: number;
  readonly lanes: readonly LaneLabel[];
  readonly hidden: number;
}

// UI to engine. Compare view: the other tracks to compare with this one, by the ids of their
// lanes entries. Temporary, not a setting.
export interface CompareMessage {
  readonly type: "compare";
  readonly ids: readonly number[];
}

// UI to engine. Compare view: draw the sum as if every compared track were shifted and flipped
// as the analysis suggests. Temporary, not a setting.
export interface PreviewFixMessage {
  readonly type: "previewFix";
  readonly on: boolean;
}

export type UiToEngineMessage =
  | HelloMessage
  | SetSettingMessage
  | ViewportMessage
  | FreezeMessage
  | PanMessage
  | ArmMessage
  | ViewMessage
  | CompareMessage
  | PreviewFixMessage;

export type EngineToUiMessage =
  SnapshotMessage | StateChangedMessage | ErrorMessage | LanesMessage;
