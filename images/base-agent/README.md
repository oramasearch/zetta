# `images/base-agent` — the Orama base model as a model image

[`Dockerfile`](Dockerfile) packs one published version of the base agent checkpoint
([`OramaSearch/base-agent-0.6b`](https://huggingface.co/OramaSearch/base-agent-0.6b), private;
model card [`docs/agent/cards/models/base-agent.md`](../../docs/agent/cards/models/base-agent.md))
into the private image `oramasearch/base-agent-0.6b:<version>`, for example `:v16`.

**Why it exists** ([lake#246](https://github.com/oramasearch/lake/issues/246),
[lake#799](https://github.com/oramasearch/lake/issues/799)): a native agent created in lake without
a model runs on the base model inside its workspace Zetta. The Orama operator gives every workspace
Zetta this image as `spec.models[{name: base-agent}]`; an init container copies `/model` to
`/data/models/base-agent` on the workspace volume before the node starts, and lake points native
agents at that path. Workspace namespaces deny egress by default, so the checkpoint arrives as an
image the kubelet pulls with the workspace's pull Secret, not as a download from the pod. The
operator side is orama-operator's `docs/zetta-models.md`.

## The contract

- `/model/` holds the checkpoint files: `config.json`, the weights (`model.safetensors`, or
  `model.safetensors.index.json` and its shards), and the tokenizer (`tokenizer.json`, or
  `vocab.json` + `merges.txt`), plus whatever else the version folder has. The build fails if the
  required ones are missing.
- `/bin/sh` exists (busybox), because the operator's copy script runs in this image.
- Every file is readable by uid 10001, the Zetta node's user.
- **A tag is never moved.** The operator replaces the copy on a volume only when the image string
  changes, so a new checkpoint is a new tag (`v17`), and the operator's default
  (`lakeDefaultBaseModelImage` in `internal/controller/lake_defaults.go`) moves with it, rolled dev,
  testing, staging, prod.

## Building a version

The context is a directory with one folder, `checkpoint/`. Take the files from the local
checkpoint first (root `CLAUDE.md`, *Checkpoints live in `data/checkpoints/`*), and from the Hub
only when they are not on disk:

```sh
ctx=$(mktemp -d)
# local: the same files the Hub's v16/ folder holds
cp -R data/checkpoints/base-agent-0.6b-interaction-v16 "$ctx/checkpoint"
# or from the Hub (private; needs `hf auth login` with access to OramaSearch)
#   hf download OramaSearch/base-agent-0.6b --include 'v16/*' --local-dir "$ctx/hub"
#   mv "$ctx/hub/v16" "$ctx/checkpoint"
```

Images are built on the cluster BuildKit, never with Docker on the Mac (liquid-bridge decision 33;
the `liquid-dev` skill has the lane): `buildctl build --frontend dockerfile.v0 --local
context="$ctx" --local dockerfile=images/base-agent --opt platform=linux/amd64 --output
type=image,name=docker.io/oramasearch/base-agent-0.6b:v16,push=true`.

**Before the first push, create the Docker Hub repository private** (`POST /v2/repositories/` with
`"is_private": true`): a first push creates it public. Afterwards an anonymous
`curl -s -o /dev/null -w '%{http_code}' https://hub.docker.com/v2/repositories/oramasearch/base-agent-0.6b/`
must answer `404` (umbrella `CLAUDE.md`, *Every service is its own private repo*).

## Size

The fp32 checkpoint is about 2.2 GB, so the image is too, and it is copied onto every workspace
volume (lake's default is 5 GiB). A bf16 copy of the same weights is half that, and the engine
loads `BF16` safetensors (`src/core/compute/safetensors.cpp`), widening them to fp32 in memory, so
it is a possible later step that changes the volume cost, not the memory one.
