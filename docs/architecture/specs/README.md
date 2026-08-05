# AI Project specs

The abstraction hierarchy for building AI systems — the structure behind
[#948](https://github.com/oramasearch/zetta/issues/948). Authored outside this repo and copied in,
because a plan whose source lives in a Downloads folder is not reproducible.

- [`ai-project-system-design.md`](ai-project-system-design.md) — the reasoning. Evals vs benchmarks,
  where RLVR sits relative to both, how an eval becomes a verifiable reward, and the level-by-level
  derivation of the hierarchy.
- [`ai-project-full-spec.md`](ai-project-full-spec.md) — the settled spec: principles, the full
  hierarchy, the component reference, the RLVR integration point, and the generator config object.
- [`diagrams/`](diagrams/) — **generated**, not hand-edited. The DOT source is extracted from the
  spec; re-render after editing it:

  ```sh
  cd docs/architecture/specs/diagrams && for f in *.dot; do dot -Tsvg "$f" -o "${f%.dot}.svg"; done
  ```

| Diagram | Shows |
|---|---|
| `AIProjectSystem` | the containment tree from ContextSnapshot down |
| `RLVRIntegration` | where verifiable reward attaches to the hierarchy |
| `GeneratorFlow` | how a configuration becomes a generated system |

**Naming has wiggle room.** These names need not match the engine's existing types. The
reconciliation table — and the open Experiences design — live in
[`docs/plans/ai-project-abstractions.md`](../../plans/ai-project-abstractions.md).
