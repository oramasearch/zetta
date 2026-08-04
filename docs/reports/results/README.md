# Results

**Client-facing** reports: what a customer or prospect asked for, what we achieved, and how.
The sibling reports in [`docs/reports/`](../README.md) are internal validation runs — this
folder is what someone outside the company reads.

That difference is the whole point of separating them, and it sets the rules:

- **State what is not working, by name.** A results report that only lists wins is
  marketing, and the reader discovers the gap at the worst possible moment. Every report
  here carries a "what is not working" section and a "what we still need from you" section.
- **Separate verified from built.** "Works", "works in our test harness", and "built but
  unverified" are three different claims. Say which one applies.
- **Every number carries its method and its date.** A number with no method is an
  assertion; the value of one decays.
- **Diagrams are generated, not drawn.** Graphviz `.dot` sources live in
  [`diagrams/`](diagrams/) beside the rendered `.svg`, so a diagram can never drift from
  what it documents.
- **Say plainly whether customer data was involved.** Where the answer is "none", say so.

## Reports

- [motive-churn-results-2026-08-04.md](motive-churn-results-2026-08-04.md) — churn for
  Motive: the table shape reconstructed from their documentation when the extract could not
  be moved, a stand-in dataset measured at 0.756 held-out AUC with no single column above
  0.642, a connector-landed dataset now feeding the pipeline that trains on it, two traps
  found in their data, two defects found in ours, and the one open defect blocking a live
  walkthrough.
  Diagrams: [flow](diagrams/motive-churn-flow.svg) ·
  [signal structure](diagrams/motive-dataset-signal.svg)

## Rendering the diagrams

```
dot -Tsvg diagrams/<name>.dot -o diagrams/<name>.svg
```

Colour is status, never decoration — green verified end to end, blue verified in-process
only, amber built but unverified, grey not built, red blocked.
