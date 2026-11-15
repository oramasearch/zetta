---
name: perplexity
description: Generate a long, well-structured Perplexity deep-dive research prompt from a topic, for the user to run in Perplexity (Pro "Deep Research" / Reasoning). Use when the user asks for a "perplexity query", a "deep dive" prompt, or a thorough research prompt on a subject — especially one comparing how specific systems or libraries solve a problem. Produces one copy-pasteable prompt; optionally runs it via the perplexity MCP tools.
---

# perplexity — deep-dive query generator

Turn a topic into a single, comprehensive Perplexity research prompt the user
can paste into Perplexity (ideal for Pro "Deep Research" / Reasoning modes). The
deliverable is the prompt itself — long, specific, and structured to elicit a
thorough, well-cited answer.

## What makes a strong deep-dive prompt

Include these (adapt to the topic):

1. **Role + goal + depth** — "Act as <expert>. I want a deep, technical
   understanding of <topic>, enough to <decide/build>." State the reader's level
   so it skips basics.
2. **Concrete scope** — enumerate the subtopics and questions; *name* the
   specific systems/libraries/papers to compare. Specificity beats breadth.
3. **Comparisons** — ask for side-by-side tables across the named systems on the
   dimensions that matter (design, API, performance, trade-offs).
4. **Mechanics + examples** — ask how things actually work under the hood, with
   minimal code/pseudocode where it clarifies.
5. **Trade-offs, pitfalls, footguns** — explicitly request failure modes and
   "what people get wrong."
6. **Decision guidance** — ask for a recommendation framework for the user's
   stated context.
7. **Sources + recency** — require authoritative primary sources (official docs,
   source code, papers, talks) with citations; prefer recent material and note
   version/date sensitivity.
8. **Output shape** — request a short executive summary first, then sections and
   tables.

## Instructions

1. **Gather topic + context** (what they're building/deciding, their level, any
   systems to compare). If the user already supplied these, infer and proceed —
   don't re-ask.
2. **Draft the prompt** from the template below, filling every bracket with
   real, topic-specific detail (actual library names, subsystems, questions).
   Longer and more specific is better for deep research.
3. **Output ONLY the finished prompt** in one fenced code block (trivially
   copy-pasteable), then a one-line note on which Perplexity mode to use.
4. **Offer to run it** via the perplexity MCP tools if they'd rather get the
   answer inline: `perplexity_research` (slow, multi-source, thorough),
   `perplexity_reason` (step-by-step analysis), `perplexity_ask` (quick).

## Template

```
Act as a [domain expert]. I want a rigorous, deep-dive understanding of [TOPIC],
detailed enough to [GOAL]. Assume I am [LEVEL]; skip introductory hand-holding.

Cover, in depth:
- [subtopic 1 — specific question]
- [subtopic 2 ...]
- How each of these systems solves it, with specifics — [SYSTEM A], [SYSTEM B],
  [SYSTEM C]: design approach, key APIs/abstractions, and the rationale.
- A comparison table across [SYSTEMS] on [dimensions].
- How it works under the hood ([mechanics]); include minimal code where it helps.
- Trade-offs, common pitfalls, and footguns.
- A decision framework for my context: [CONTEXT].

Requirements: cite authoritative primary sources (official docs, source code,
papers, conference talks); prefer recent material and flag version/date
sensitivity; lead with a short executive summary, then sections and tables.
```

## Best practices
- Research prompts cover the public landscape only: never put confidential, customer or
  unpublished material into a query sent to an external service.
- Name real systems and real subsystems — vague prompts get vague answers.
- Tie the "decision framework" to the user's actual project/constraints.
- Keep it to one prompt; don't fragment a deep dive into many small questions.

## Notes
This is a generic productivity skill — move it to `~/.claude/skills/perplexity/`
to use it across all projects.
