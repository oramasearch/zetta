# Operations

Operator-facing runtime diagnostics and tuning guidance.

- [Memory accounting](memory.md) — process RSS, allocator truth, subsystem
  ownership, growth diagnosis, periodic logs, and allocator tuning (#1163).
  Audit ownership covers compact segment replication indexes and caches, not
  replicated catalog events (#1164); replication counters re-baseline after
  catalog migration and cannot wrap on subtraction (#1167).
