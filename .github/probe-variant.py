# Temporary: switches one part of the trace renderer off, for timing only.
import sys

path = "ui/src/render/trace-renderer.ts"
s = open(path).read()
never = "if (performance.now() < 0) "
variant = sys.argv[1]
if variant == "baseline":
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
else:
    sys.exit(f"unknown variant {variant}")
open(path, "w").write(s)
print(f"variant {variant}")
