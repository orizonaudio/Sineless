// Control message types exchanged as JSON over the native channel (AD-3, DESIGN 6.1).
// Every message is an object with a `type` field holding one of these values.

export const MessageType = {
  hello: "hello",
  snapshot: "snapshot",
  setSetting: "setSetting",
  stateChanged: "stateChanged",
  error: "error",
  credit: "credit",
} as const;
export type MessageType = (typeof MessageType)[keyof typeof MessageType];
