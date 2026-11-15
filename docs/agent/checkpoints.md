# Registered agent checkpoints

Native agent serving accepts exact checkpoint selectors registered by the node operator
(#1411). A workspace writer can choose an installed model, but cannot turn
`agent.model` into an arbitrary local directory probe.

The default registry contains `base-agent` and the legacy selector
`/data/models/base-agent`, both pointing to `/data/models/base-agent`. This preserves
the model mount used by [orama-operator#730](https://github.com/oramasearch/orama-operator/pull/730)
and the [base-model image](https://github.com/oramasearch/zetta/pull/1344).

## Operator configuration

Set `ZETTA_AGENT_CHECKPOINTS` before starting the node. Its JSON object extends or
overrides the defaults; each key is an exact selector and each value an absolute
directory containing a downloaded checkpoint:

```sh
export ZETTA_AGENT_CHECKPOINTS='{"customer-agent":"/data/models/customer-agent"}'
```

Use `"model":"customer-agent"` when creating or updating an agent. To retain a legacy
model string, register that literal string as a key. Neither Hugging Face repository IDs
nor `model:<uuid>` values load unless explicitly registered. Downloading a model with
`zetta pull` does not authorize it for serving. Invalid registry JSON, non-string values,
relative directories and embedded NUL bytes fail closed.

Checkpoint directories and their files must be controlled by the operator and remain
immutable while serving. Publish a replacement checkpoint while the node is stopped.
The registry is node-local configuration, never a replicated entity or a workspace API;
configure every serving node with the same selectors and its own local directories.

## Serving checks

`checkpoint_directory` looks up the selector before any filesystem operation. Unknown
selectors return `agent.model: checkpoint is not registered`; they are never normalized,
URL-decoded, appended to a root or passed to a loader. A registered directory must exist
and its canonical absolute path must match the configured path without symlink components.
Top-level symlinks and special files are refused. A sharded safetensors index may name
only regular files directly in that directory, without separators, traversal or NUL bytes.

`respond` performs these checks before the tokenizer cache, `config.json`, the GPU runner
or the CPU fallback. REST/MCP `chat` (including proposal turns), `agent_run` and desktop chat share
that function. `ModelCache::get` and `load` repeat the check before cache hits and loads,
covering desktop preloading too. Cache entries use the resolved canonical directory,
so aliases do not create separate model copies. Removing a registration prevents reuse
of its cached tokenizer or weights.

Agent CRUD and replication retain model strings without reading the filesystem. A record
can therefore be stored before its model is installed locally, and serving fails closed
until registration succeeds. There is no new API wire format; the existing model string
now selects a registered checkpoint rather than naming an unchecked directory.

## Verification and scope

`AgentCheckpoints.*` uses a tiny native checkpoint to prove that an existing unregistered
directory never reaches a tokenizer, model cache or inference runner, while a registered
model serves through CPU and injected GPU paths. It also covers revoked warm caches,
directory and file symlinks, escaping shard names, malformed operator configuration and
the built-in selectors. API tests cover persisted selectors through `chat` and `agent_run`.

Low-level tokenizer and model-loading APIs still accept paths for trusted local training,
checkpoint tooling and methodology implementations. They do not consume `AgentRecord.model`;
this change governs served agent selection. The registered trees' immutability is a local
filesystem trust requirement, not a lock against concurrent modification by a local writer.
