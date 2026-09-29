# Memory implementation

`accounting.cpp` aggregates probes and confines platform allocator/process reads:
glibc and `/proc` on Linux, malloc zones and Mach on macOS. It also owns compact
log formatting and the guarded Linux `malloc_trim(0)` adapter. See
[memory accounting](../../../docs/ops/memory.md).
