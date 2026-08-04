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
- **Never publish an in-sample metric as a result.** Held-out and in-sample go in the same
  table, labelled, with the held-out figure first. An in-sample number quoted alone is the
  most common way a model flatters itself, and it survives into a slide unchallenged.
- **Every number carries its method and its date.** A number with no method is an
  assertion; the value of one decays.
- **Diagrams are generated, not drawn.** Graphviz `.dot` sources live in
  [`diagrams/`](diagrams/) beside the rendered `.svg`, so a diagram can never drift from
  what it documents.
- **Say plainly whether customer data was involved.** Where the answer is "none", say so.

## Reports

- [motive-churn-results-2026-08-04.md](motive-churn-results-2026-08-04.md) — churn for
  Motive: the pipeline runs end to end on a 1,500-account communications dataset built to
  the source table's shape. The **pipeline** is the result: ingest → featurize → train →
  TreeSHAP → rank, with escalations first and account size near zero. The **model** generalizes at
  15,000 accounts — held-out AUC 0.726 against 0.770 in-sample, a 0.04 gap. At 1,500 the
  same pipeline gave 0.619 held out against 0.919 in-sample, so sample size was the limit
  rather than the configuration.
  Diagrams: [pipeline](diagrams/motive-churn-flow.svg) ·
  [signal structure](diagrams/motive-dataset-signal.svg)

## Rendering the diagrams

```
dot -Tsvg diagrams/<name>.dot -o diagrams/<name>.svg
```

Colour is status, never decoration — green verified end to end, blue verified in-process
only, amber built but unverified, grey not built, red blocked.
