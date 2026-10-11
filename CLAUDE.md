@AGENTS.md

## Claude Code

- For a platform, SDK, host or standard problem, use the `prior-art` skill before
  writing code.
- Before merging, equalising or deriving numeric values, use the `changing-values` skill.
- Before pushing, run the `reviewer` subagent on your diff and address or answer each
  finding.
- Area-specific rules load from `.claude/rules/` when you work in `engine/`, `editor/`,
  `ui/` or `protocol/`.
- A hook blocks `git push` to `main` (`.claude/hooks/no-push-to-main.mjs`).
