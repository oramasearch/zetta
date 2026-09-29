# Native tests

GoogleTest exercises the headless core and native loopback transports. Build
with at most six jobs and run one suite at a time. Audit lifecycle regressions
cover idle windows, class-specific retention, explicit purge, and peer trimming
(#1169). The audit proof worker is a
separate executable so each replicated node has independently measured resident
and restart peak memory (#1164). See [the proof runbook](../docs/testing/audit-replication.md).
Study lifecycle tests cover running-study refusal, non-promoted Run and metric
removal, promoted lineage retention, restart persistence, and REST/MCP parity
(#1158).
