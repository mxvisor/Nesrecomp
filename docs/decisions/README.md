# Architecture Decision Records

Short records of *why* the project does something a certain way, so a future
session does not re-propose a rejected alternative. One file per decision:
`NNNN-short-title.md`. Never rewrite an accepted ADR — supersede it with a new
one and set the old one's status to `Superseded by NNNN`.

Template:

```markdown
# NNNN — Title

- Status: Accepted | Superseded by NNNN
- Date: YYYY-MM-DD

## Context
What problem forced a choice.

## Decision
What we do.

## Rejected alternatives
What we considered and why not.

## Consequences
What this makes easier / harder; what to watch for.
```

| ADR | Decision |
|-----|----------|
| [0001](0001-lag-sequence-primary-sync-metric.md) | Lag sequence (then RAM hash) is the demo-sync metric, not the framebuffer |
| [0002](0002-gpl-fceux-oracle-out-of-tree.md) | GPL FCEUX sources live only in gitignored `nogpl/` |
| [0003](0003-separate-fceux-backend.md) | FCEUX-faithful playback is a separate `--interp=fceux` backend; beam stays default |
| [0004](0004-fm2-discovery-mesen-verification.md) | FM2 for address discovery, Mesen for correctness verification |
