# autograd — zetta's own reverse-mode autodiff

`zetta::autograd` is a native C++26 reverse-mode automatic-differentiation engine —
the foundation the RLVR model (#555) trains on. It exists so zetta owns its training
math end to end: **no foreign runtime** (ADR-17 — not libtorch, not a Python stack),
which keeps the binary lean, the build simple, and lets us write our own **fused
kernels** instead of inheriting a framework's. The design is micrograd-shaped (a
value graph with per-node backward closures), extended from scalars to **tensors**,
which is what a transformer needs.

## How it works

- **`Node`** — one value in the graph: `data`, `shape`, accumulated `grad`, its
  `parents`, and a `backward` closure that scatters this node's gradient to its
  parents.
- **`Tensor`** — a shared handle to a `Node`. Build the graph forward with the ops;
  call **`backward()`** on the scalar loss to seed its gradient to 1 and propagate
  through the graph in reverse topological order, accumulating each parameter's
  gradient.
- **Correctness** — every op is verified by **gradient checking**: the analytic
  gradient from `backward()` must match a finite-difference estimate. That is the
  standard proof an autograd op is right.

## Device-agnostic execution (shipped)

The reference kernels are plain host loops, but the heavy op (matmul, forward and
backward) routes through the `compute::ComputeDevice` seam: `set_compute_device()`
points the autograd at a `CpuDevice`, `CudaDevice`, or `MlxDevice` (thread-local, so
each cluster node picks its own), and the *same graph* runs on it. Verified: the
`Transformer` forward produces matching logits on the CPU and the MLX GPU
(`AutogradDeviceAgnostic.TransformerForwardCpuEqualsMlx`), so one distributed model
spans CPU + GPU + MLX nodes — the cluster uses what each node has. Gradients are host
floats, so the distributed allreduce (already built for the RLVR loop) combines them
across heterogeneous devices.

## Pages

- (this README — overview; `tensor.hpp` carries the engine + ops as they ship)

## Shipped

- **`tensor.hpp`** — `Node`, `Tensor`, `backward()`; ops `matmul` and `sum`.
