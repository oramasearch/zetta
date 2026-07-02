# Experience — UX, DX, AX

Orama Zetta serves three audiences deliberately, and each has a designed,
documented experience. The shared spine: **one engine entry point per
capability, one governance model, one observability system** — every
experience is a rendering of the same registry and the same `system.*` read
model, so nothing any audience sees can drift from what the cluster actually
did.

| Experience | Audience | Spec |
|---|---|---|
| [ux.md](ux.md) | Humans operating the cluster — the desktop lens + the planned web interface | gui/architecture.md · gui/design-system.md · control-surfaces.md |
| [dx.md](dx.md) | People building zetta — the disclosure network, conventions, skills, traced suite | architecture/coding-conventions.md · observability.md · the rule-cards |
| [ax.md](ax.md) | Agents driving the cluster — MCP parity + prompts, llms.txt endpoints, the spec index, traces threaded back | api/registry-design.md · api/adding-a-capability.md |

Cross-cutting: [serving.md](serving.md) — models and indexes as governed HTTP
endpoints (cluster-internal by default), the `serve` step's full meaning.

When a surface or convention changes, update the affected experience page in
the same commit — these pages are the contract each audience reads.
