// Entry point: picks the transport and starts the UI.
import { MockTransport } from "./bridge/mock-transport.ts";
import { NativeTransport, type Transport } from "./bridge/transport.ts";
import { startSineless } from "./start.ts";
import "./styles.css";

// Inside the plugin the native channel exists; in a desktop browser the mock stands in. WebView2
// delivers native messages as events, which are handed to the bridge like WKWebView's calls.
const webview = window.chrome?.webview;
webview?.addEventListener("message", (event) => {
  if (typeof event.data === "string") {
    window.orizonBridge?.receive(event.data);
  }
});
const channel = window.webkit?.messageHandlers["orizon"] ?? webview;
const transport: Transport =
  channel !== undefined
    ? new NativeTransport({ channel, receiver: window })
    : new MockTransport();
startSineless(transport);
