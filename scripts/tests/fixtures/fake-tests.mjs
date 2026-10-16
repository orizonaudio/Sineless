/* global process, setInterval */
// Stands in for a test binary built with mutants: the mutant whose variable is set decides how
// the run ends, as its id says: "kill:" fails a test, "survive:" passes, "hang:" never ends.
const id = Object.keys(process.env).find((key) =>
  /^(kill|survive|hang):/u.test(key),
);
if (id?.startsWith("hang:")) {
  setInterval(() => undefined, 1000);
} else {
  process.exit(id?.startsWith("kill:") ? 1 : 0);
}
