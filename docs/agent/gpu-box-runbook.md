# Runbook — training and CUDA development on a GPU box

Two GPU targets, for two different jobs. Read [Which box](#which-box) before provisioning anything —
**a rented GPU cannot train this model yet**, and the reason is not obvious.

Owner: the agent + compute domains. Tracking: [#920](https://github.com/oramasearch/zetta/issues/920)
(epic) · [#921](https://github.com/oramasearch/zetta/issues/921) (driver) ·
[#923](https://github.com/oramasearch/zetta/issues/923) (`CudaQwenModel`) ·
[#924](https://github.com/oramasearch/zetta/issues/924) (memory).

## Which box

| Job | Where | Why |
|---|---|---|
| **Train the agent today** | **the Mac** (MLX) | The only working training path. See [training-runbook.md](training-runbook.md). |
| **Develop CUDA kernels** (#922) | **server2**, the build box | Needs *a* CUDA GPU, not a fast one. RTX 3080 is `sm_86` — same architecture as an A10. Free. |
| **Train on CUDA** | rented A10 / A100 | **Blocked on #923.** Nothing trains on Linux until `CudaQwenModel` exists. |
| **9B LoRA** (#864) | rented A100 40 GB | 40 GB is the unlock; 24 GB is tight. |
| **9B full fine-tune** (#865) | 2x H100 80 GB | Needs a sharded path that does not exist yet. |

> **Why a rented GPU does not help yet.** The SFT driver is entirely
> `#if defined(ZETTA_WITH_MLX)` — Apple silicon only — and `next_token_loss` exists **only** in
> `mlx_qwen.cpp`. The native autograd `QwenModel` has `forward` and `generate` but no loss or
> training driver. So on Linux there is currently no code path that trains this model, on any GPU.
> Provisioning one before #923 buys a machine that can build the engine and run the data-engine
> CUDA backend, and cannot fine-tune anything.

## Instance choice (Lambda)

Ampere or newer, native BF16, single card. Our checkpoints are stored **BF16** and both safetensors
readers widen to fp32 on load.

| Instance | Rate | Verdict |
|---|---|---|
| **1x A10 (24 GB PCIe)** | $1.29/hr | **Default.** GA102 / `sm_86` — *identical architecture to the build box's RTX 3080*, so one code path covers local dev and rented runs. 24 GB clears the fp32-AdamW requirement (9.6 GB before activations) outright. |
| **1x A100 (40 GB SXM4)** | $1.99/hr | Headroom for iteration, and the right card for 9B LoRA. |
| 2x H100 (80 GB SXM5) | $8.38/hr | Correct for a 9B full fine-tune — **after** sharded training exists. |
| 8x A100 / 8x B200 | $15.92+/hr | Premature: no multi-GPU path. Pays for idle GPUs. |
| **8x Tesla V100 (16 GB)** | $6.32/hr | **Avoid.** Volta has **no native BF16** — only FP16, with a narrower exponent range and loss-scaling fragility. Paying to acquire a numerics problem we do not have. |

**Cost is not the constraint at 0.6B.** A 3000-step run is ~72 min on the Mac at 1.45 s/step;
an A10 should roughly halve that. The entire run is about **$1**. Pick on fit, not price — and
remember these bill hourly, so the real failure mode is a forgotten instance, not the training cost.

## Governance

The #918 corpus is **synthetic and plan-derived — no customer data** (see the
[data card](cards/datasets/base-agent.md)). Training the base agent on rented hardware therefore
does not touch the "your data never leaves your environment" posture, which governs *customer* data
in *customer* environments.

**The day we fine-tune on a customer's node ([#853](https://github.com/oramasearch/zetta/issues/853)),
rented GPUs stop being an option.** That is not a preference; it is the product's contract.

## Provision and build

```sh
# 1. Provision in the Lambda console (A10 or A100), then:
ssh ubuntu@<instance-ip>

# 2. Toolchain. Lambda images ship the CUDA toolkit; zetta needs CMake 3.28+ and a
#    C++26 compiler (Clang 17+ / GCC 14) -- the repo script picks one.
git clone --recurse-submodules git@github.com:oramasearch/zetta.git
cd zetta && ./scripts/install-deps.sh

# 3. Build with the CUDA backend ON (it is OFF by default).
cmake -S . -B build -G Ninja -DZETTA_ENABLE_CUDA=ON
cmake --build build -j

# 4. Confirm the GPU is actually visible to the toolkit, not just present.
nvidia-smi --query-gpu=name,memory.total,driver_version,compute_cap --format=csv
./build/tests/zetta-tests --gtest_filter='*Cuda*'
```

The submodule recursion matters (`automerge-cpp` nests its own, and provides nlohmann/json).
`oramacore-cpp` is private — a checkout without it still builds green via `ZETTA_WITH_ORAMACORE`.

**Expect the CUDA tests to be sparse.** `cuda_device.cu` implements the data-engine primitives
(`gemm`, `reduce`, `scan`, `sort`, `histogram`, `gather`/`scatter`), which is what exists today.

## Once #923 lands — running SFT on CUDA

The corpus is generated natively, so nothing needs uploading except the repo:

```sh
# Generate the gated corpus on the box (deterministic -- same spec, same corpus).
ZETTA_CORPUS_OUT=data/corpora/interaction-corpus-v1.jsonl \
  ./build/tests/zetta-tests --gtest_filter='InteractionContract.WriteCorpusJsonl'

# Pull the base weights natively (ADR-17 -- no python, no curl).
ZETTA_QWEN_DIR=$(./build/bin/zetta pull Qwen/Qwen3-0.6B)

# Train. SAVE_EVERY is not optional: a run that only saves at the end loses
# everything to an interruption, which is exactly how ~90 steps were lost (#919).
ZETTA_QWEN_DIR=$ZETTA_QWEN_DIR \
ZETTA_CORPUS=data/corpora/interaction-corpus-v1.jsonl \
ZETTA_SFT_EXAMPLES=6030 ZETTA_SFT_STEPS=3000 ZETTA_SFT_SAVE_EVERY=250 \
ZETTA_CHECKPOINT_OUT=data/checkpoints/base-agent-0.6b-interaction-v1 \
ZETTA_SFT_LOG=data/sft-logs/interaction-v1.jsonl \
  ./build/tests/zetta-tests --gtest_filter='*SftBaseAgentOnCorpus'
```

Then evaluate against the **canary** (the 763 held-out cases over Projects whose numbers the model
never saw) and write the dated report — the report is the deliverable, the checkpoint is not:

```sh
ZETTA_QWEN_DIR=data/checkpoints/base-agent-0.6b-interaction-v1 \
ZETTA_EVAL=data/corpora/interaction-corpus-v1.jsonl.canary \
  ./build/tests/zetta-tests --gtest_filter='*EvalGraderPassRate'
```

Retrieve the checkpoint (~2.2 GB) with `rsync -a`, and **destroy the instance**.

## Hard rules

- **A training run must never be able to take down its host.** A run exhausted memory on the Mac and
  the kernel panicked on a watchdog timeout (#919). Cap device memory and fail loudly with "OOM at
  step N" instead. This is #924 and it applies to every backend.
- **Do not run a node and a training run on the same box.** server2 is also the local k0s cluster.
- **A CUDA-saved checkpoint must load on the CPU path.** A checkpoint only its own backend can read
  is a trap, and the whole point is training in one place and serving in another (#923).
- **No foreign runtime** (ADR-17). CUDA is a `ComputeDevice` backend and kernels we write — never a
  bridge to PyTorch, never a subprocess. `zetta-core` must never link CUDA.

## See also

[training-runbook.md](training-runbook.md) (the Mac/MLX path) ·
[interaction-contracts.md](interaction-contracts.md) ·
[cards/](cards/README.md) — read the model + data card before changing or diagnosing a model ·
[../reports/](../reports/README.md) — every training or eval run writes a dated report here
