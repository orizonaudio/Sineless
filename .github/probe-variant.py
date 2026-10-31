# Temporary: switches one part of the trace renderer off, for timing only.
import sys

path = "ui/src/render/trace-renderer.ts"
s = open(path).read()
never = "if (performance.now() < 0) "
variant = sys.argv[1]
if variant in ("baseline", "baseline-2", "baseline-3"):
    pass
elif variant == "no-composite":
    s = s.replace("gl.drawArrays(gl.TRIANGLES, 0, 3);", never + "gl.drawArrays(gl.TRIANGLES, 0, 3);")
elif variant == "no-clear":
    first = s.index("gl.clear(gl.COLOR_BUFFER_BIT);")
    second = s.index("gl.clear(gl.COLOR_BUFFER_BIT);", first + 1)
    s = s[:second] + never + s[second:]
elif variant == "no-segments":
    s = s.replace("gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, frame.pointCount);", never + "gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, frame.pointCount);")
elif variant == "no-upload":
    s = s.replace("gl.texSubImage2D(", never + "gl.texSubImage2D(")
elif variant == "antialias":
    s = s.replace("antialias: false", "antialias: true")
elif variant == "ring":
    def r(a, b):
        global s
        assert s.count(a) == 1, a[:60]
        s = s.replace(a, b)
    r("  private readonly frameTexture: WebGLTexture;\n  private textureRowsAllocated = 0;",
      "  private frameTexture: WebGLTexture;\n  private textureRowsAllocated = 0;\n  private readonly ring: { texture: WebGLTexture; rows: number }[] = [];\n  private ringAt = 0;")
    r("    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);\n",
      "    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);\n    this.ring.push({ texture: this.frameTexture, rows: 0 });\n    for (let k = 0; k < 2; k++) {\n      const t = gl.createTexture();\n      gl.bindTexture(gl.TEXTURE_2D, t);\n      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);\n      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);\n      this.ring.push({ texture: t, rows: 0 });\n    }\n")
    r("    gl.bindTexture(gl.TEXTURE_2D, this.frameTexture);\n    if (rows > this.textureRowsAllocated) {",
      "    this.ringAt = (this.ringAt + 1) % 3;\n    const slot = this.ring[this.ringAt]!;\n    this.frameTexture = slot.texture;\n    this.textureRowsAllocated = slot.rows;\n    gl.bindTexture(gl.TEXTURE_2D, this.frameTexture);\n    if (rows > this.textureRowsAllocated) {")
    r("      this.textureRowsAllocated = rows;\n", "      this.textureRowsAllocated = rows;\n      slot.rows = rows;\n")
elif variant == "no-upload-no-composite":
    s = s.replace("gl.texSubImage2D(", never + "gl.texSubImage2D(")
    s = s.replace("gl.drawArrays(gl.TRIANGLES, 0, 3);", never + "gl.drawArrays(gl.TRIANGLES, 0, 3);")
elif variant == "trivial-fragment":
    a = "  float coverage = clamp(lineHalf + 0.5 - d, 0.0, 1.0);"
    assert s.count(a) == 1
    s = s.replace(a, "  float coverage = 0.5;")
elif variant == "composite-1px":
    a = "gl.drawArrays(gl.TRIANGLES, 0, 3);"
    assert s.count(a) == 1
    s = s.replace(a, "gl.enable(gl.SCISSOR_TEST); gl.scissor(0, 0, 1, 1); gl.drawArrays(gl.TRIANGLES, 0, 3); gl.disable(gl.SCISSOR_TEST);")
elif variant in ("early-out", "early-out-no-upload"):
    a = "  vec3 rgb = vec3(0.0);"
    assert s.count(a) == 1
    s = s.replace(a, "  if (coverage[0] + coverage[1] + coverage[2] + coverage[3] == vec4(0.0)) {\n    discard;\n  }\n" + a)
    if variant == "early-out-no-upload":
        s = s.replace("gl.texSubImage2D(", never + "gl.texSubImage2D(")
elif variant in ("split", "split-fill"):
    def r(a, b):
        global s
        assert s.count(a) == 1, a[:70]
        s = s.replace(a, b)
    r("in uvec2 nextValues;\n", "in uvec2 nextValues;\nin uvec2 previousValues;\n")
    a = s.index("  int corner = gl_VertexID % 6;")
    b = s.index("  gl_Position = vec4(x / canvas.x * 2.0 - 1.0, y / canvas.y * 2.0 - 1.0, 0.0, 1.0);\n}`;")
    s = s[:a] + """  int part = gl_VertexID / 6;
  int corner = gl_VertexID % 6;
  bool far = corner == 2 || corner == 3 || corner == 5;
  bool upper = corner == 1 || corner == 4 || corner == 5;
  int i = gl_InstanceID;
  float pad = lineHalf + 1.0;
  float lo;
  float hi;
  bool present = point(true, hereValues, lo, hi);
  float lo2;
  float hi2;
  bool joined = point(i + 1 < count, nextValues, lo2, hi2);
  float loP;
  float hiP;
  bool before = point(i > 0, previousValues, loP, hiP);
  piece = i;
  here = vec2(lo, hi);
  next = joined ? vec2(lo2, hi2) : vec2(1.0, -1.0);
  fillPart = part == 0 ? 1 : 0;
  float xa = pixelX(float(i) + offset);
  float xb = joined ? pixelX(float(i + 1) + offset) : xa;
  vec2 c = vec2(0.0);
  if (part == 0 && present && joined) {
    float low = clamp(min(lo, lo2), 0.0, canvas.y);
    float high = clamp(max(hi, hi2), 0.0, canvas.y);
    c = vec2(far ? xb : xa, upper ? high : low);
  } else if ((part == 1 || part == 2) && present && joined) {
    float bottom = -4.0 * canvas.y;
    float top = 5.0 * canvas.y;
    vec2 a = vec2(xa, part == 1 ? hi : lo);
    vec2 b = vec2(xb, part == 1 ? hi2 : lo2);
    if (!((a.y > top && b.y > top) || (a.y < bottom && b.y < bottom))) {
      vec2 a2 = clipped(a, b, bottom, top);
      vec2 b2 = clipped(b, a, bottom, top);
      vec2 along = b2 - a2;
      float len = length(along);
      vec2 dir = len > 0.0 ? along / len : vec2(1.0, 0.0);
      vec2 across = vec2(-dir.y, dir.x);
      c = (far ? b2 + dir * pad : a2 - dir * pad) + (upper ? across : -across) * pad;
    }
  } else if (part == 3 && present && !(before && joined)) {
    float low = clamp(lo - pad, 0.0, canvas.y);
    float high = clamp(hi + pad, 0.0, canvas.y);
    c = vec2(far ? xa + pad : xa - pad, upper ? high : low);
  }
  float x = c.x;
  float y = c.y;
""" + s[b:]
    r("flat out vec2 next;\nvoid main() {", "flat out vec2 next;\nflat out int fillPart;\nvoid main() {")
    r("flat in vec2 next;\nuniform float spacing;", "flat in vec2 next;\nflat in int fillPart;\nuniform float spacing;")
    if variant == "split-fill":
        a2 = "void main() {\n  vec2 p = gl_FragCoord.xy;\n  float xa = pixelX(float(piece) + offset);"
        r(a2, a2 + """
  if (fillPart == 1) {
    float xb0 = pixelX(float(piece + 1) + offset);
    float f0 = (p.x - xa) / spacing;
    if (!(f0 >= 0.0 && f0 <= 1.0 && p.y >= mix(here.x, next.x, f0) && p.y <= mix(here.y, next.y, f0))) {
      discard;
    }
    vec4 full = vec4(0.0);
    full[channel] = 1.0;
    coverage0 = attachment == 0 ? full : vec4(0.0);
    coverage1 = attachment == 1 ? full : vec4(0.0);
    coverage2 = attachment == 2 ? full : vec4(0.0);
    coverage3 = attachment == 3 ? full : vec4(0.0);
    return;
  }""")
    r("gl.drawArraysInstanced(gl.TRIANGLES, 0, 6, frame.pointCount);", "gl.drawArraysInstanced(gl.TRIANGLES, 0, 24, frame.pointCount);")
    r("    const size = values.byteLength + 8;", "    const size = values.byteLength + 16;")
    r("    gl.bufferSubData(gl.ARRAY_BUFFER, 0, values);", "    gl.bufferSubData(gl.ARRAY_BUFFER, 8, values);")
    r('    const next = program.attribute("nextValues");', '    const next = program.attribute("nextValues");\n    const previous = program.attribute("previousValues");')
    s = s.replace("for (const attribute of [here, next]) {", "for (const attribute of [here, next, previous]) {")
    r("      const offset = values.byteOffset - start;", "      const offset = values.byteOffset - start + 8;\n      gl.vertexAttribIPointer(previous, stride, gl.UNSIGNED_INT, stride * 4, offset - stride * 4);")
else:
    sys.exit(f"unknown variant {variant}")
open(path, "w").write(s)
print(f"variant {variant}")
