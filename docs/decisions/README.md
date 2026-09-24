# Decisions Log

Short, dated records of standing decisions that don't belong in the root or
per-module `CLAUDE.md` files — usually because they resolve one of the root
`CLAUDE.md` "Open Questions" items, or because the rationale is long enough that
inlining it in a module file would bloat context loaded on every task.

These files are **not** auto-loaded by Claude Code. Link to the relevant one from the
root or module `CLAUDE.md` when it should be surfaced automatically.

## Format

One file per decision: `NNNN-short-title.md`, incrementing.

```markdown
# NNNN: Title

Date: YYYY-MM-DD
Status: decided | superseded by NNNN

## Decision

## Why

## Alternatives considered
```
