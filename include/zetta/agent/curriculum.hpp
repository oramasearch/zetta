#pragma once

#include "zetta/agent/admission_gate.hpp"

// The base-agent training curriculum: authored, gated seed tasks (#866). Each is a
// trainable, *discriminating* game in the blender-rl sense — it ships a reference
// solution that passes its grader and negative controls that fail (see the
// rlvr-training-data skill). The frontier-model generators grow the corpus around
// these seeds, and every generated task passes this same `admit_corpus` gate before it
// is trusted. This header exposes the native seed cases the corpus is grown from.
namespace zetta::agent {

// Sample #1 — Charles's churn game (Motive UC-1): the improvement over his current
// LLM-pattern-matching n8n. The required end-state is a *correct* churn solution —
// connect the source, engineer leakage-free features, train a gradient-boosted model
// with SHAP explanations, and serve calibrated risk scores with a score-vs-outcome
// calibration loop. The reference satisfies all of that; the negative controls (an
// LLM-only pipeline like his, a leaky one, a no-calibration one) each fail.
AdmissionCase churn_solution_case();

}  // namespace zetta::agent
