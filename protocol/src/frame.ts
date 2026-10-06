// Binary layout of a display frame (AD-3): a little-endian header, then float32 data for each
// view present in `views`, in ChannelView order. Each view holds `pointCount` points: a min/max
// pair per point in FrameMode.minMax, one sample per point in FrameMode.raw.

export type FieldType = "u8" | "u16" | "u32" | "f32";

export interface FrameField {
  readonly name: string;
  readonly type: FieldType;
}

export const frameHeaderFields = [
  { name: "sequence", type: "u32" },
  { name: "mode", type: "u8" },
  { name: "flags", type: "u8" },
  { name: "views", type: "u8" },
  { name: "reserved", type: "u8" },
  { name: "pointCount", type: "u32" },
  { name: "samplesPerPoint", type: "f32" },
] as const satisfies readonly FrameField[];

export type FrameHeaderFieldName = (typeof frameHeaderFields)[number]["name"];

// Bits of the `flags` field.
export const FrameFlag = { triggered: 1, frozen: 2, roll: 4 } as const;

const fieldSizes: Record<FieldType, number> = { u8: 1, u16: 2, u32: 4, f32: 4 };

export interface FieldLayout {
  readonly name: string;
  readonly type: FieldType;
  readonly offset: number;
}

// Offsets follow the field order. Every field must be naturally aligned.
export function layoutFields(fields: readonly FrameField[]): {
  fields: FieldLayout[];
  size: number;
} {
  let offset = 0;
  const laidOut = fields.map((field) => {
    const size = fieldSizes[field.type];
    if (offset % size !== 0) {
      throw new Error(
        `Frame field '${field.name}' is not aligned to ${String(size)} bytes`,
      );
    }
    const layout = { name: field.name, type: field.type, offset };
    offset += size;
    return layout;
  });
  return { fields: laidOut, size: offset };
}

export const frameHeader = layoutFields(frameHeaderFields);
