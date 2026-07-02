# include/zetta — public API rules

This tree is zetta-core's **public surface**. Per-file annotations live in
[docs/architecture/code-map.md](../../docs/architecture/code-map.md) — update
it in the same commit when adding, moving, or renaming a header.

- **No heavy/backend type leaks into a public header:** no SDL/ImGui anywhere;
  `compute/cuda_device.hpp` is CUDA-free (no `<cuda_runtime.h>`); the
  oramacore-cpp engine hides behind a pImpl; nlohmann/json is allowed (it is
  part of the core's vocabulary).
- **Backend names stay behind the seams.** `boost::capy` / `boost::corosio`
  appear only in `runtime.hpp` / `runtime/net.hpp` (and their adapters in
  `src/core/`). Consumers program against `zetta::runtime` / the seam
  interfaces (`StorageEngine`, `ComputeDevice`, `IndexBackend`, `Provisioner`,
  `Dialect`, `StepExecutor`, `LanguageFrontend`, …) — extending zetta means
  implementing one of these, not widening a consumer.
- **Handles, not raw pointers:** lookups return `std::shared_ptr` (empty on
  miss); `std::unique_ptr` for sole ownership; raw pointers only for
  RAII-managed C-API handles and optional non-owning back-references
  (`ReplicationManager*`, `AuditSink*`).
- **Full, descriptive names** — no 1–2 letter or cryptic abbreviations for
  namespaces, types, functions, variables, or template parameters. Text is
  `std::string` / `std::string_view`.
- **Non-throwing boundaries:** public parse/validate entry points return
  `std::optional` / status types with a path-qualified reason; nothing throws
  across the zetta-core seam.
- Header comments carry the owning issue refs (`#NNN`) and point at the
  matching `docs/<domain>/` page — keep both current when the surface changes.
