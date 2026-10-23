# Experience — UX, DX, AX

Orama Zetta serves three audiences deliberately, and each has a designed,
documented experience. The shared spine: **one engine entry point per
capability, one governance model, one observability system** — every
experience is a rendering of the same registry and the same `system.*` read
model, so nothing any audience sees can drift from what the cluster actually
did.

| Experience | Audience | Spec |
|---|---|---|
| [ux.md](ux.md) | Humans operating the cluster — the desktop lens + the Orama Studio web shell, with script-safe fallback data | gui/architecture.md · gui/design-system.md · control-surfaces.md |
| [design-sync.md](design-sync.md) | The Studio design import ledger — per-sync pin, what moved, what was skipped and why | the studio-design-sync skill · gui/design-system.md |
| [dx.md](dx.md) | People building zetta — the disclosure network, conventions, skills, traced suite | architecture/coding-conventions.md · observability.md · the rule-cards |
| [ax.md](ax.md) | Agents driving the cluster — MCP parity, SQL-governed search (#1403), discoverable Study inputs and validation, one-call page listings, governed lifecycle actions, prompts, llms.txt endpoints, the spec index, traces, and audit maintenance | api/registry-design.md · api/adding-a-capability.md |

Cross-cutting: [serving.md](serving.md) — models and indexes as governed HTTP
endpoints (cluster-internal by default), the `serve` step's full meaning.

**One mode spans all three experiences: using zetta through Claude Code (or
another coding agent).** Users operate the cluster conversationally (UX),
developers build zetta with agents as the normal mode (DX), and the agent in
the middle is exactly who the tool surface serves (AX) — the `intent` field,
prompts, traces, and GUI live surfacing are the same loop seen from three
seats.

When a surface or convention changes, update the affected experience page in
the same commit — these pages are the contract each audience reads.
