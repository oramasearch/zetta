# Research: surviving a crash while staying memory-first (mmap-backed segments)

> **Provenance.** One Perplexity (`perplexity_ask`, high context) run,
> 2026-09-27, after D17 (events survive restarts and crashes, but topics are
> memory-only for the most part). **Status:** background; describes no zetta code.

## What a shared file mapping survives

Ring segments can live in files mapped with `mmap(MAP_SHARED)`. Writing an
event is then a memory write, and the kernel owns the dirty pages.

| Failure | Survives? |
|---|---|
| process crash: `SIGKILL`, segfault, OOM kill of the process | **yes**: the page cache belongs to the kernel, which writes it back later [3] |
| kernel panic, power loss, node or VM failure | **no**, unless the pages were already written back or `msync`/`fdatasync`ed |
| a half-written event at the moment of the crash | present but torn: records need framing + a checksum, and recovery truncates the torn tail |

`msync(MS_SYNC)` / `fdatasync` narrow the machine-failure window at a cost;
`MS_ASYNC` is not a barrier. Chronicle Queue works this way: plain appends to
mapped files, and an explicit `sync()` when power-loss durability is wanted [1][7].

## Pitfalls

- **cgroup v2 charges the page cache to the pod.** Mapped file pages count in
  `memory.stat` `file` / `file_mapped` / `file_dirty`, against `memory.max`.
  Dirty pages cannot be dropped until written back, so heavy write pressure can
  stall or OOM the pod [3]. The detected memory budget (D11) must include them.
- **`SIGBUS`** if the disk fills or a mapped file is truncated under the mapping:
  preallocate segment files, never truncate a mapped file.
- **Latency:** a page fault on a cold mapping can be disk I/O; writeback under
  pressure stalls writers.
- **Kubernetes:** a PersistentVolume outlives the pod; restarting on the same node
  finds everything the kernel wrote back. A node failure loses what was only in
  that node's page cache [2][5].
- **macOS:** same idea through the unified buffer cache; use its sync calls for
  durability, don't assume Linux semantics.

## Sources

[1] https://chronicle.software/insights/blogs/comparing-approaches-to-durability-in-low-latency-messaging-queues ·
[2] https://kubernetes.io/docs/concepts/storage/volumes/ ·
[3] https://www.kernel.org/doc/html/latest/admin-guide/cgroup-v2.html ·
[5] https://kubernetes.io/docs/concepts/storage/persistent-volumes/ ·
[7] https://chronicle.build/queue/
