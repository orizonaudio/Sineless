// Throwaway: which WebGL renderer the browser gives the page.
import { test } from "vitest";

test("gpu info", () => {
  const gl = document.createElement("canvas").getContext("webgl2");
  const info = gl?.getExtension("WEBGL_debug_renderer_info");
  const describe = {
    webgl2: gl !== null,
    vendor: info ? gl?.getParameter(info.UNMASKED_VENDOR_WEBGL) : gl?.getParameter(gl.VENDOR),
    renderer: info ? gl?.getParameter(info.UNMASKED_RENDERER_WEBGL) : gl?.getParameter(gl.RENDERER),
    userAgent: navigator.userAgent,
  };
  throw new Error("RESULT " + JSON.stringify(describe));
});
