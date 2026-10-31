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
else:
    sys.exit(f"unknown variant {variant}")
open(path, "w").write(s)
print(f"variant {variant}")
