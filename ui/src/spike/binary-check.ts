// T-005 spike: decodes a base64 payload and checks the test pattern (byte i == i mod 256).

export function decodeBase64(base64: string): Uint8Array {
  const text = atob(base64);
  const bytes = new Uint8Array(text.length);
  for (let i = 0; i < text.length; i++) {
    bytes[i] = text.charCodeAt(i);
  }
  return bytes;
}

export function hasTestPattern(bytes: Uint8Array): boolean {
  return bytes.every((value, index) => value === index % 256);
}
