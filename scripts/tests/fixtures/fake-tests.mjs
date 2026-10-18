/* global process, setInterval */
// Stands in for a test binary built with mutants: the mutant whose variable is set decides how
// the run ends, as its id says: "kill:" fails a test, "survive:" passes, "hang:" never ends. Each
// argument names one more prefix this binary fails on, as one test binary notices a mutant that
// another misses.
const failsOn = ["kill", ...process.argv.slice(2)];
const prefixes = [...failsOn, "survive", "hang"].map((name) => `${name}:`);
const id = Object.keys(process.env).find((key) =>
  prefixes.some((prefix) => key.startsWith(prefix)),
);
if (id?.startsWith("hang:")) {
  setInterval(() => undefined, 1000);
} else {
  process.exit(failsOn.some((name) => id?.startsWith(`${name}:`)) ? 1 : 0);
}
