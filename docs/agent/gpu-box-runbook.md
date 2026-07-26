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

Measured on a Lambda `gpu_1x_a100_sxm4` (2026-07-26): A100-SXM4-40GB, `sm_80`, driver 580.105.08,
CUDA 12.8, 30 vCPU, 216 GB RAM, Ubuntu 24.04.4, CMake 3.28.3.

### Put the checkout on the LOCAL disk, never the network filesystem

Lambda mounts a persistent filesystem at `/lambda/nfs/<name>` (virtiofs). It is not for building:

| Mount | Device | Measured write |
|---|---|---|
| `/` and `$HOME` | `/dev/vda1` ext4 | **2.1 GB/s** |
| `/lambda/nfs/<name>` | virtiofs | **49.5 MB/s** |

**43x difference.** `$HOME` is already on the local disk, so the default location is correct — just
never point a build or a checkout at `/lambda/nfs`. There is no separate `/dev/nvme*`: the local disk
is virtio-blk, NVMe-backed on the host, and `lsblk` reporting `ROTA 1` for it is a virtio artefact,
not a spinning disk.

The tradeoff to be deliberate about: the local disk is **ephemeral**. Anything worth keeping — a
checkpoint, a validation report — gets copied off before the instance is destroyed.

### Toolchain — the stock image cannot build zetta

Four gaps, all of which the first real Linux+CUDA build hit in order. Do these before configuring:

```sh
# 1. Build tools + the libraries libpqxx and SDL need.
sudo apt-get update && sudo apt-get install -y \
  ninja-build ccache lld zlib1g-dev libssl-dev pkg-config \
  libpq-dev libstdc++-14-dev gcc-14 g++-14

# 2. A C++26 Clang. The image ships only g++-13, which cannot do C++26, and
#    scripts/install-deps.sh installs the distro clang, which is also too old.
wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh && chmod +x /tmp/llvm.sh
sudo /tmp/llvm.sh 22          # 20, 21, 22 are available for noble

# 3. A CMake that KNOWS Clang 22. Ubuntu 24.04 ships 3.28, whose compiler-feature
#    table predates Clang 20, so it rejects cxx_std_26 as "not known to CXX
#    compiler Clang 22.1.8" -- a CMake limitation, not a compiler one.
sudo snap install cmake --classic          # 4.4.0; the Mac runs 4.3.4
```

**Match the Mac's compiler.** Local development uses Homebrew clang 22.1.8 and `clang-22` on noble is
also 22.1.8 — same C++26 support level, so a build green locally does not fail here over library
features. `libstdc++-14-dev` matters because clang-22 targets GCC 14's install dir; without it the
very first link fails with `cannot find -lstdc++`.

### Build

```sh
/snap/bin/cmake -S . -B build -G Ninja \
  -DZETTA_ENABLE_CUDA=ON \
  -DCMAKE_CXX_COMPILER=clang++-22 -DCMAKE_C_COMPILER=clang-22 \
  -DCMAKE_CUDA_COMPILER=clang++-22 \
  -DCMAKE_CUDA_ARCHITECTURES=80 \
  -DSDL_UNIX_CONSOLE_BUILD=ON
/snap/bin/cmake --build build -j 30
```

Two of those flags are not obvious:

- **`-DCMAKE_CUDA_COMPILER=clang++-22` — let Clang compile the `.cu` itself.** nvcc enforces a host
  compiler whitelist that our toolchain outruns: CUDA 12.8 rejects host Clang >= 20 outright
  (*"unsupported clang version! clang version must be less than 20"*). Measured on this box:

  | Combination | Result |
  |---|---|
  | nvcc + clang-22 host | **fails** the version check |
  | nvcc + clang-19 host | works |
  | **clang-22 compiling CUDA directly** | **works** |

  Clang-as-CUDA-compiler keeps one compiler across the whole build and needs no host compiler at
  all. `-DCMAKE_CUDA_HOST_COMPILER=clang++-19` under nvcc is the alternative.
- **`-DSDL_UNIX_CONSOLE_BUILD=ON`** — SDL3 aborts configure without X11/Wayland headers. A headless
  box wants neither, and the tests link `zetta-core` only, never the GUI. This is SDL's documented
  escape rather than installing a desktop stack.

`-DCMAKE_CUDA_ARCHITECTURES` must match the card: **80** for A100, **86** for A10 / RTX 3080. Both are
Ampere with native BF16, but they are different `-arch` targets — build for the card you are on.

### Verify

```sh
nvidia-smi --query-gpu=name,memory.total,driver_version,compute_cap --format=csv
./build/tests/zetta-tests --gtest_filter='*Cuda*:*ComputeDevice*'
```

**Check the timings, not just the greens.** `CudaDevice.RoundTripAndOperatorsWhenPresent` skips at
runtime when no GPU is visible, and a skip still reports as a pass. On the A100 it takes ~300 ms; a
0 ms result means it skipped and nothing touched the GPU.

### Two latent bugs the first CUDA build exposed

Both had been invisible because no Mac and no CI job enables CUDA:

- `tests/compute/test_compute_device.cpp` included `cuda_device.hpp` **inside** the file's anonymous
  namespace, nesting the header's own `namespace zetta::compute` a level deeper so its unqualified
  references stopped resolving. Fixed by hoisting the include to file scope.
- `tests/autograd/test_autograd.cpp` used `std::log` with no `<cmath>` — libc++ provides it
  transitively, libstdc++ does not.

Worth expecting more of this class as CUDA coverage grows: a second standard library and a second
backend are both good at finding assumptions the first one hid.

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
