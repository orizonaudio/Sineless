// The author's answers to earlier AI review findings on a pull request, for the review's verifiers
// (.github/workflows/ai-review.yml, "Collect the author's answers"): a verifier keeps a finding an
// answer covers only if it shows the answer wrong (.claude/agents/review-verifier.md).
//
// Usage: node scripts/ai-review-answers.ts <review comments> <conversation comments>
// Each file is a list of GitHub comments, all pages merged (`gh api --paginate ... | jq -s add`,
// which gives null for a pull request with none). Prints the answers as JSON.
import { readFileSync } from "node:fs";

interface Comment {
  id?: number;
  // eslint-disable-next-line @typescript-eslint/naming-convention -- GitHub's REST API names it.
  in_reply_to_id?: number | null;
  user?: { login?: string } | null;
  // eslint-disable-next-line @typescript-eslint/naming-convention -- GitHub's REST API names it.
  author_association?: string;
  path?: string;
  line?: number | null;
  // eslint-disable-next-line @typescript-eslint/naming-convention -- GitHub's REST API names it.
  original_line?: number | null;
  body?: string;
}

export interface Reply {
  author: string;
  body: string;
}

export interface Thread {
  file: string;
  // Where the finding was; null for a comment on a whole file.
  line: number | null;
  finding: string;
  replies: Reply[];
}

export interface Answers {
  threads: Thread[];
  comments: Reply[];
}

const bot = "github-actions[bot]";
// The workflow's inline comment for an Important finding starts with this.
const important = "**Important:** ";
// Anyone can comment on a public pull request; only people with write access answer for it.
const answering = new Set(["OWNER", "MEMBER", "COLLABORATOR"]);

const list = (value: unknown): Comment[] =>
  Array.isArray(value) ? (value as Comment[]) : [];
const login = (comment: Comment) => comment.user?.login ?? "";
const answers = (comment: Comment) =>
  login(comment) !== bot &&
  answering.has(comment.author_association ?? "") &&
  (comment.body ?? "").trim() !== "";
const reply = (comment: Comment): Reply => ({
  author: login(comment),
  body: comment.body ?? "",
});

export function collect(review: unknown, conversation: unknown): Answers {
  const comments = list(review);
  const threads = comments
    .filter(
      (root) => login(root) === bot && (root.body ?? "").startsWith(important),
    )
    .map((root) => ({
      file: root.path ?? "",
      line: root.line ?? root.original_line ?? null,
      finding: (root.body ?? "").slice(important.length).split("\n")[0] ?? "",
      replies: comments
        .filter((c) => c.in_reply_to_id === root.id && answers(c))
        .map(reply),
    }))
    .filter((thread) => thread.replies.length > 0);
  return {
    threads,
    comments: list(conversation)
      .filter(
        (c) =>
          answers(c) && !(c.body ?? "").trimStart().startsWith("/ai-review"),
      )
      .map(reply),
  };
}

if (process.argv[1]?.endsWith("ai-review-answers.ts") === true) {
  const [review, conversation] = process.argv
    .slice(2)
    .map((path): unknown => JSON.parse(readFileSync(path, "utf8")));
  process.stdout.write(
    `${JSON.stringify(collect(review, conversation), null, 2)}\n`,
  );
}
