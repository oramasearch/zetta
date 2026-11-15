# The use-case catalog

**The AI project is the product's central item** (ADR-25): most AI projects
fail, so the product's real job is the data-project management discipline
that makes them succeed — problem → right data → correct-by-construction
model → governed deploy → monitor and iterate. **A use case is a generic
AI-project type** (ADR-26): the same template any team runs on its own data;
the engine is general (GBDT today; RL, LLM fine-tuning behind the same seams)
— the catalog grows, the engine carries forward.

- **UC-1 churn** is the template and the proof: featurize (point-in-time) →
  train → explain → rank, plus the optional action layer (playbook + narrator).
  [../use-cases/](../use-cases/README.md) is the worked example end to end.
- **The next use cases are the same shape**: forecasting, lead scoring,
  triage, anomaly/fraud, predictive maintenance, next-best-action — a
  different label, featurize spec, trap, and driver→action map on the same
  engine.
- **Growing the catalog**: the upstream flow (deep-research prompt →
  blueprint UC-N artifact → issues → worktree plan) is amaro-bridge's
  add-use-case skill + [runbook](https://github.com/oramasearch/amaro-bridge/blob/main/docs/runbooks/adding-a-use-case.md);
  the zetta-side build recipe is
  [../use-cases/creating-a-use-case.md](../use-cases/creating-a-use-case.md)
  (the two laws: compose-don't-invent; name the Contoso test).
- **In the product**, the catalog is the AI Solution Library
  ([../pipeline/solution-catalog.md](../pipeline/solution-catalog.md)) behind
  one-button deploy — and later the personalization trio ranks it per
  customer (FEAT-11 Auto-Suggest → FEAT-12 Auto-RL → FEAT-13 Personalized
  MCP, deferred by design).
