# Temporary: varies the trace renderer and logs the playhead test's frame count, for timing only.
import sys

def patch(path, a, b):
    s = open(path).read()
    assert s.count(a) >= 1, (path, a[:60])
    open(path, "w").write(s.replace(a, b))

root, variant = sys.argv[1], sys.argv[2]
test = root + "/ui/tests/app/playhead.test.ts"
patch(test, "    const frames = await drawnFrames();\n",
      "    const frames = await drawnFrames();\n    console.log(`PROBE frames ${frames.length}`);\n")
renderer = root + "/ui/src/render/trace-renderer.ts"
never = "if (performance.now() < 0) "
if variant == "skip":
    patch(renderer, "  vec3 rgb = vec3(0.0);\n  float alpha = 0.0;\n",
          "  if (coverage[0] + coverage[1] + coverage[2] + coverage[3] == vec4(0.0)) {\n    discard;\n  }\n  vec3 rgb = vec3(0.0);\n  float alpha = 0.0;\n")
elif variant == "no-composite":
    patch(renderer, "    gl.drawArrays(gl.TRIANGLES, 0, 3);\n    if (sweep",
          "    " + never + "gl.drawArrays(gl.TRIANGLES, 0, 3);\n    if (sweep")
elif variant == "no-traces":
    patch(renderer, "gl.drawArraysInstanced(gl.TRIANGLES, 0, 24, frame.pointCount);",
          never + "gl.drawArraysInstanced(gl.TRIANGLES, 0, 24, frame.pointCount);")
else:
    assert variant == "baseline", variant
