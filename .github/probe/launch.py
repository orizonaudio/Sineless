# Temporary: lets the probe set Chromium's launch options and headless mode from the environment.
path = "vitest.config.ts"
s = open(path).read()
a = """              ...(chromium === undefined
                ? {}
                : { launchOptions: { executablePath: chromium } }),"""
assert a in s
s = s.replace(a, """              ...(process.env["PROBE_LAUNCH"] !== undefined
                ? { launchOptions: JSON.parse(process.env["PROBE_LAUNCH"]) }
                : {}),""")
b = "            headless: true,"
assert b in s
s = s.replace(b, '            headless: process.env["PROBE_HEADFUL"] !== "1",')
open(path, "w").write(s)
