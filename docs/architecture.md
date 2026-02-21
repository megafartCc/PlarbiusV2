# Architecture

## Layering

1. `core`: shared scalar types and constants.
2. `game`: generic extensive-form game interfaces.
3. `games/*`: concrete game implementations (Kuhn now, Leduc next).
4. `cfr`: training algorithms and infoset storage.
5. `infra`: logging and platform utilities.
6. `policy`: normalized average-policy export/load.
7. `eval`: exploitability and expected-value selfplay evaluation.
8. `apps`: executable entrypoints.
9. `tests`: unit + smoke validation.

## Design Principles

- Keep game logic and training logic decoupled through interfaces.
- Keep infoset storage algorithm-agnostic.
- Prefer deterministic behavior for reproducible experiments.
- Keep runner apps thin; put logic into library modules.
- Keep evaluation read-only and parallelize only pure traversal paths.

## Scale Path

1. Baseline: full-tree CFR+ on tiny games.
2. Speed: external-sampling MCCFR and batched traversals.
3. Realism: action/card abstraction for larger poker trees.
4. Robustness: exploitability measurement and regression tests.
5. Production: checkpointing, resumable training, distributed workers.
