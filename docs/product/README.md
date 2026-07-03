# Product docs

What Orama Zetta *is* — the product framing, naming, and the working decisions that shape every
engineering choice in this repo. The central idea: the **AI project** is the product's central item
(most AI projects fail; the product's job is data-project management), built use-case-first from the
wtm `ORAMA-ZETTA` blueprint. Read here before any work that touches product scope, naming, or the
use-case catalog; the per-use-case detail lives in [../use-cases/](../use-cases/README.md).

- [orama-zetta.md](orama-zetta.md) — **read first**: what Orama Zetta is + the locked working decisions (naming, value-first messaging, AI projects as the central item, UC-1 churn, the wtm blueprint as source of truth).
- [control-surfaces.md](control-surfaces.md) — **drive programmatically, watch visually**: the MCP + REST control surface as the primary way to drive a cluster (agents first, full GUI parity), the GUI as the observability lens, and why that ordering validates the cluster while we build it (API-6/API-8; sets up FEAT-13).
