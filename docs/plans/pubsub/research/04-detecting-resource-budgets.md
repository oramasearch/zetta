# Research: detecting the memory, CPU and disk budget from the environment

> **Provenance.** One Perplexity (`perplexity_ask`, high context) run,
> 2026-09-27, after Steve asked that defaults be auto-selected from the
> environment instead of guessed. Plus a read of zetta's own
> `src/core/cluster/node_metrics.cpp`. **Status:** background; describes no
> pub/sub code.

## What zetta reads today

`node_metrics.cpp` reads **host** figures: `hw.memsize` on macOS,
`/proc/meminfo` (`MemTotal`, `MemAvailable`) on Linux, and `statvfs` on a path
for disk. It reads **no cgroup**, so inside a pod it reports the host's memory,
not the pod's limit.

## Linux containers and Kubernetes

- **Find the cgroup:** `/proc/self/cgroup` gives the path; `/proc/self/mountinfo`
  gives where the cgroup filesystem is mounted (not always `/sys/fs/cgroup`).
- **cgroup v2:** `memory.max` (hard limit; `max` = none), `memory.high`
  (throttling threshold, not a cap), `memory.current` (usage), `cpu.max`
  (`quota period`, e.g. `150000 100000` = 1.5 CPUs; `max` = none), `cpu.weight`
  (relative weight, not a CPU count) [4][8].
- **cgroup v1:** `memory.limit_in_bytes` (a huge sentinel means unlimited),
  `cpu.cfs_quota_us` / `cpu.cfs_period_us` (negative quota = none).
- **Ancestors can be tighter** than the process's own cgroup: take the minimum
  up the tree.
- **`/proc/meminfo` is the host,** never the container's budget [6].
- **Kubernetes requests are not in cgroups.** Limits are; requests only become
  `cpu.weight` and memory protection hints. To know the request, the pod must
  expose it through the **Downward API** (`resourceFieldRef`: `requests.memory`,
  `requests.cpu`, `limits.*`) as environment variables [1].
- **Disk:** cgroups carry no disk budget. `statvfs` on the data directory's
  filesystem; in a pod, the mounted volume, and any `ephemeral-storage` limit
  exposed through the Downward API.
- **CPU:** report both the cpuset / affinity (which cores) and the quota (how
  much time).

## What other runtimes default to

| Runtime | Detects | Default budget |
|---|---|---|
| JVM | container limits (`UseContainerSupport`, on by default) | heap = 25% of the limit (`MaxRAMPercentage`); `-XshowSettings:system` prints what it saw [2][6] |
| Go (`automemlimit`) | cgroup limit | `GOMEMLIMIT` = 90% of the limit |
| .NET | container limit | GC heap hard limit = 75% of the limit |
| macOS | no cgroups | `hw.memsize` total, `host_statistics64` for used [3][7] |

The lesson everyone converged on: **log the raw inputs, where each came from
(cgroup limit, Downward API request, host fallback), and the budget chosen.**

## Sources

[1] https://kubernetes.io/docs/concepts/configuration/manage-resources-containers/ ·
[2] https://www.ibm.com/docs/en/sdk-java-technology/8?topic=options-xx-usecontainersupport ·
[3] https://psutil.io/api/ ·
[4] https://www.kernel.org/doc/html/latest/admin-guide/cgroup-v2.html ·
[6] https://aws.amazon.com/blogs/containers/jvm-memory-cpu-and-classpath-best-practices-for-java-containers-on-aws/ ·
[7] https://eclecticlight.co/sysctl-information/ ·
[8] https://docs.kernel.org/admin-guide/cgroup-v2.html
