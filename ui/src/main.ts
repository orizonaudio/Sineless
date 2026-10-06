// T-005 spike page: proves the native message channel, binary delivery and keyboard routing.
import { decodeBase64, hasTestPattern } from "./spike/binary-check.ts";
import { FrameRateMeter } from "./spike/frame-rate.ts";

interface NativeChannel {
  postMessage(message: string): void;
}

declare global {
  interface Window {
    orizonBridge?: {
      receive(message: string): void;
      receiveBinary(base64: string): void;
    };
    webkit?: { messageHandlers: Record<string, NativeChannel | undefined> };
  }
}

const status = document.createElement("p");
const frameRate = document.createElement("p");
const keyboardHint = document.createElement("label");
const input = document.createElement("input");
input.id = "spike-input";
keyboardHint.textContent =
  "Typing here goes to the page; elsewhere, keys go to the host: ";
keyboardHint.append(input);
document.body.append(status, frameRate, keyboardHint);

function setStatus(text: string): void {
  status.textContent = text;
}

function send(message: string): void {
  window.webkit?.messageHandlers["orizon"]?.postMessage(message);
}

window.orizonBridge = {
  receive(message) {
    setStatus(`Received: ${message}`);
  },
  receiveBinary(base64) {
    const bytes = decodeBase64(base64);
    const ok = hasTestPattern(bytes);
    setStatus(
      ok
        ? `Bridge OK: ${String(bytes.length)} bytes verified`
        : "Binary payload corrupt",
    );
    send(`${ok ? "binary-ok" : "binary-bad"}:${String(bytes.length)}`);
  },
};

setStatus("Waiting for native side...");
send("ping");

// T-006: report the requestAnimationFrame rate once per second with the page's visibility.
const meter = new FrameRateMeter();
function onFrame(timestamp: number): void {
  const rate = meter.tick(timestamp);
  if (rate !== undefined) {
    const state = `${document.visibilityState} focus=${String(document.hasFocus())}`;
    frameRate.textContent = `requestAnimationFrame: ${rate.toFixed(1)} fps (${state})`;
    send(`raf:${rate.toFixed(1)}:${state}`);
  }
  requestAnimationFrame(onFrame);
}
requestAnimationFrame(onFrame);
