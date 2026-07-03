# Positioning — the whole arc, not one slice

No incumbent shrinks the whole arc from "I have a question" to "a correct,
governed system is answering it in production." That arc is what we compress.

| We are not | Because |
|---|---|
| **Demoware** | LLM-only tools and notebooks demo well, then stall before production. Zetta's output is a deployed, gated, governed system — and it trains on your data rather than prompting a rented model. |
| **Someone else's cloud** | Zetta self-deploys into *your* environment (k8s / AWS / Azure / GCP / OCI / on-prem / laptop). The data never leaves; zero egress is a testable claim (TC-2). |
| **A web PaaS** | Same one-button deploy feel — but data/AI-native, enterprise-governed, on private infrastructure. |
| **An answer engine** | Answer engines retrieve and respond. We build, deploy, and train models and pipelines on your data — answers come with lineage, calibration, and drivers. |

**True hybrid** is a structural differentiator, not a checkbox: one equal-peer
cluster spans vendors and architectures *at once* (CPU/CUDA/ROCm/MLX/Vulkan/
SYCL/TPU/Neuron; macOS/Linux/Windows; x86/ARM; any cloud + on-prem), giving
cost (burst under a cap, spot-tolerant), flexibility (new silicon is a new
backend, not a re-architecture), and resilience (self-heal around a vendor).
Engine detail: [../architecture/true-hybrid.md](../architecture/true-hybrid.md).

Buyer shorthand (internal): *self-deploying Databricks; Railway for
private-cloud data + AI.* External claims discipline: cost-savings numbers
stay internal until measured (blueprint RSK-6); the marketing site is the
voice-of-record for public copy.
