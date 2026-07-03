# UX — user experience

Humans operating the cluster: they **watch, comprehend, and approve**; driving
is one click or one sentence, never form-filling. The GUI is the observability
lens on a cluster that is primarily driven programmatically
([control-surfaces.md](../control-surfaces.md)).

## Surfaces

- **The headed desktop app** — itself a cluster member: the spatial cluster
  view with live gauges, the algorithm-at-work animation, the graph canvas,
  master-detail panels over the `system.*` read model.
- **The web interface** (planned, [#647](https://github.com/oramasearch/zetta/issues/647))
  — the same read model + registry rendered in a browser for operators away
  from the desktop and for sharing a cluster view across a team. Strictly a
  rendering; never a fourth behavior path.

## Principles

- Structure: the view registry, the widget grammar, one status language,
  `navigate_to` + entity links ([../gui/architecture.md](../../gui/architecture.md)).
- Look + copy: the Orama design system ([../gui/design-system.md](../../gui/design-system.md))
  — sentence case, product language, coral as "you are here".
- Empty states teach (one sentence + the single next action); feedback quotes
  the engine verbatim; consent gates name exactly what would happen; long
  operations are visible jobs, never frozen frames.

## The loop

Watch what you (or your agent) asked for happen live; click through the object
graph (run → model → job → graph); export a feedback bundle when something is
wrong ([../architecture/observability.md](../../architecture/observability.md)).

Working checklist: the **gui-dev** skill. Wave epic: #627.
