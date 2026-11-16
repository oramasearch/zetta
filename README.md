# zetta

**A replicated C++26 engine for data, search and machine learning, running as equal peers.**

zetta stores data, indexes it, and trains and serves models on it, across a cluster of equal nodes
with no leader. All state is one CRDT document that every node converges on. Work runs next to the
data it reads, and the partial results are combined across nodes. It is one statically linked
binary with no runtime services to provision. It runs headless on a server, or headed with a
desktop UI.

Part of Orama Lake, the sovereign AI stack. MIT-licensed.

## What it does

- **Replicated storage:** databases, tables and rows, large objects, a keyed record store and an
  object store. Every write replicates to every node, and large bodies move peer to peer, checked by
  SHA-256. [Docs](docs/data/README.md)
- **An equal-peer cluster:** mutual TLS between nodes, joined with a join token, and no leader or
  election. [Docs](docs/cluster/README.md)
- **SQL:** a dialect-neutral query engine, with partitioned tables scanned on the nodes that own
  them, and read-only `system.*` views of live state. [Docs](docs/data/query.md)
- **Native machine learning:**
  - histogram gradient-boosted trees trained by gradient-histogram allreduce across nodes, with
    calibration, scoring and TreeSHAP explanations;
  - segmentation and the other methodologies behind one registry.

  No Python or foreign runtime. [Docs](docs/ml/README.md)
- **Pipelines:** a project's work is an execution graph. A correctness gate checks the graph before
  it deploys, and every run is recorded. [Docs](docs/pipeline/README.md)
- **Full-text search:** per-partition indexes searched by scatter-gather. [Docs](docs/indexing/README.md)
- **Distributed compute:** the `ComputeDevice` interface (CPU always, an accelerator when present),
  a work-stealing scheduler, and distributed dataframes. [Docs](docs/compute/README.md)
- **Connectors:** pull rows from Postgres, MySQL, TiDB, Snowflake and others into a partitioned
  dataset, read-only. [Docs](docs/data/README.md)
- **Governance:** key-scoped and role-based access, column masking and row security, a sealed
  identity root, and an audit trail of every call. [Docs](docs/governance/README.md)
- **One control surface:** every capability is a REST route, an MCP tool and an OpenAPI operation
  from one registry (190 `/v1` paths). [Docs](docs/api/README.md)
- **Pub/sub inside a node:** topics, inboxes, delivery options and spilling to disk; clustered
  pub/sub is not built yet. [Docs](docs/pubsub/README.md)
- **Observability:** every operation records its intent, plan, timing and result shape as spans,
  readable back as `system.*` views. [Docs](docs/architecture/observability.md)

What changed in each release, and what is knowingly still broken, is in the
[release notes](docs/releases/README.md). The latest is [`v0.6.0`](docs/releases/v0.6.0.md).

## Quickstart

With Docker, using the image built from the release tag:

```sh
docker run --rm -p 4800:4800 -p 7777:7777 -v zetta-data:/data oramasearch/zetta:v0.6.0
```

The node starts headless. It serves REST and MCP on port 4800 and peer replication on port 7777,
and logs `zetta 0.6.0: node started`. Started without `--admin-token`, it prints a generated owner
token once: save it, and call the API with `Authorization: Bearer <token>`. See
[docs/api/README.md](docs/api/README.md).

### Build from source

You need CMake 3.28+, Ninja and a C++26 Clang 17+:
- on macOS, Homebrew LLVM (`brew install llvm`);
- on Linux, `clang++-NN`.

Apple's system Clang and Ubuntu's GCC 13 are too old; the build picks a suitable compiler itself.

```sh
git clone --recurse-submodules https://github.com/oramasearch/zetta.git && cd zetta
./scripts/install-deps.sh                 # system tools (macOS / Ubuntu)
cmake -S . -B build -G Ninja && cmake --build build
cmake --build build --target zetta-tests && ctest --test-dir build --output-on-failure

./build/bin/zetta --headless              # a headless node; Ctrl-C to stop
./build/bin/zetta                         # headed, with the desktop UI (macOS)
```

`--recurse-submodules` matters: the coroutine, networking, HTTP and CRDT libraries are submodules
under `modules/`, and every one is public. The first configure also fetches SDL3, Dear ImGui,
GoogleTest and Boost, each pinned by commit or hash. Full instructions and troubleshooting are in
[docs/build.md](docs/build.md). The image's `Dockerfile` is at the repository root.

### A cluster

```sh
zetta --headless                                             # the first node prints its join command
zetta --headless --peer 192.0.2.10:7777 --join-token <hex>  # another machine joins it
```

Data created on any node converges on every node. See the nodes with
`SELECT * FROM system.nodes;`. Details: [docs/cluster/](docs/cluster/README.md).

## How it fits the stack

zetta is the engine of Orama Lake. The lake workspace stores its data in zetta and runs its
pipelines, training and search there. The Orama operator runs zetta on Kubernetes and also uses it
as its own datastore. zetta-tunnel links zetta nodes in different networks.

| Piece | What it does | Relation to zetta |
|---|---|---|
| lake | The AI workspace | uses zetta for data, training, scoring and search |
| orama-operator | Runs the stack on Kubernetes | deploys zetta clusters and stores its state in one |
| [zetta-tunnel](https://github.com/oramasearch/zetta-tunnel) | Tunnels, a mutual-TLS mesh and ingress | carries zetta peer links between networks, and links zetta's transport library |
| [liquid](https://github.com/oramasearch/liquid) | Version control, builds, packages and deploys | builds and ships the stack |

## Known limits

- **Security fixes still in review:** the remaining fixes from the pre-launch security review are in
  open pull requests and are not in `v0.6.0`
  ([#1388](https://github.com/oramasearch/zetta/issues/1388)). Read the release's *Known issues*
  before exposing a node.
- **The Studio web app is not in this release:** it is being rebuilt from its source
  ([#1526](https://github.com/oramasearch/zetta/issues/1526)). A node serves its built-in
  server-rendered UI at `/`.
- **Pub/sub is node-local:** topics are not replicated across nodes yet.
- **`score` over a table on a multi-member cluster** scores only the answering member's rows
  ([#1533](https://github.com/oramasearch/zetta/issues/1533)).
- **`WHERE NOT (x = 5)`** matches rows where `x` is null
  ([#1473](https://github.com/oramasearch/zetta/issues/1473)).
- **A headed node does not quit on Ctrl-C in the terminal** (macOS): close the window or press
  Cmd+Q.

The full list for each release is in its [release notes](docs/releases/README.md).

## Documentation

- [docs/README.md](docs/README.md): every domain, with its own index.
- [docs/architecture/code-map.md](docs/architecture/code-map.md): the source tree, file by file.
- [schemas/openapi.json](schemas/openapi.json): the generated API reference. A running node also
  serves it at `/v1/openapi.json`.
- [docs/testing/](docs/testing/README.md): the test suites, including the in-app suite catalog.
- [docs/releases/](docs/releases/README.md): the release notes.

## Community

- Bugs and requests: [issues](https://github.com/oramasearch/zetta/issues).
- Security reports: send them privately, as [SECURITY.md](SECURITY.md) describes. Please don't open a
  public issue.

## Contributing

Contributions are welcome. [CONTRIBUTING.md](CONTRIBUTING.md) says how to build, test and open a pull
request against `dev`. Contributions come in under the [Contributor License Agreement](CLA.md), which
you sign once.

Everyone taking part follows the [code of conduct](CODE_OF_CONDUCT.md).

## License

MIT. See [LICENSE](LICENSE). Third-party components keep their own licenses, listed in
[THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES).

"Orama Lake", "liquid", "lake", "zetta" and their logos are trademarks of OramaSearch, Inc.
