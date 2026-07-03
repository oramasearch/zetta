# Compute docs

The native distributed accelerated-compute engine: the `ComputeDevice` operator seam (new hardware =
a new backend, nothing else), the work-stealing scheduler + job coordinator that split a
`ComputeJob` across cores and nodes (SPMD, collectives at round boundaries), and the
compute-near-data rule — an op runs over the partitions a node already owns, exchanging only
partials over the wire. Read here for anything touching device backends, job scheduling/lifecycle,
autoscaling, or near-data execution; the ops themselves live in [../ddf/](../ddf/ddf.md) and
[../ml/](../ml/gbdt.md).

- [compute-device.md](compute-device.md) — the `ComputeDevice` operator interface + typed device-buffer memory model + the `zetta-compute` target and optional CUDA backend (ADR-27 / #359, #363).
- [cpu-device.md](cpu-device.md) — the always-built reference backend: every operator on host memory, multi-threaded via `runtime::parallel_for` (`CpuDevice`, #360).
- [device-registry.md](device-registry.md) — per-node device enumeration + capability report + backend selection (accelerator-first, CPU fallback) (`DeviceRegistry`, #362).
- [work-stealing-scheduler.md](work-stealing-scheduler.md) — the task pool: idle/faster workers and nodes steal pending work, elastic mid-job membership (`WorkStealingScheduler` / `DistributedScheduler`, #366/#367).
- [job-coordinator.md](job-coordinator.md) — the single entry compute algorithms submit work through: tasks on the scheduler, collectives at round boundaries, job lifecycle (`JobCoordinator`, #372/#579).
- [compute-jobs.md](compute-jobs.md) — the `ComputeJob` record: one unit of distributed compute (featurize/train/explain/score/ddf), lifecycle + audit + JSON boundary (ARC-11 / ENT-31).
- [compute-near-data.md](compute-near-data.md) — a distributed op runs over a node's OWNED partitions; bytes-moved ≈ partials, not ≈ dataset (`load_local_partitions`, #516, TC-11).
- [scaling-policy.md](scaling-policy.md) — the per-job reward objective an autoscaler optimizes: cost / runtime target / data shape, inside hard bounds (`ScalingPolicy`, ADR-29 / #370).
- [job-autoscaler.md](job-autoscaler.md) — the control loop: observe a running job, score its policy, add/remove nodes via the `Provisioner` with hysteresis (`JobAutoscaler`, #371).
- [job-checkpoint.md](job-checkpoint.md) — burst-job eviction tolerance: durable checkpoints + resume-from-last after a spot eviction (`JobCheckpoint` / `run_resumable_rounds`, RULE-5 / #468).
- [pipeline-instrumentation.md](pipeline-instrumentation.md) — live pipeline instrumentation as read views: `system.pipeline_jobs` / `system.collective_rounds`, ephemeral, feeds the GUI animation (#589).
