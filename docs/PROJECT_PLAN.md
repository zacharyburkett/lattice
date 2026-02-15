# Lattice Project Plan (Draft)

This plan is intentionally front-loaded with correctness and observability
before aggressive optimization.

## Phase 0: Contract Lock-In

Deliverables:

- Freeze scope for v1 API subset.
- Confirm storage model and defer semantics.
- Define benchmark scenes and success thresholds.

Exit criteria:

- Proposal and API sketch accepted.

## Phase 1: Core Runtime Skeleton

Deliverables:

- `lt_world_t` lifecycle and allocator integration.
- Entity table with generation-safe handles.
- Component registry with metadata validation.

Exit criteria:

- Unit tests for create/destroy/alive checks.
- Stale-handle behavior validated.

## Phase 2: Archetypes and Chunks

Deliverables:

- Archetype keying and lookup.
- Chunk allocation and SoA column layout.
- Entity move path for add/remove component.

Exit criteria:

- Unit tests for structural moves and data preservation.
- No leaks in ASan run.

## Phase 3: Query Engine

Deliverables:

- Query descriptor compilation to matching archetypes.
- Chunk iteration API.
- Basic access-mode conflict validation (debug checks).

Exit criteria:

- Query correctness tests across include/exclude combinations.
- Microbench baseline collected for hot loop iteration.

## Phase 4: Deferred Command Buffer

Deliverables:

- Begin/end defer mode.
- Ordered command buffering and flush.
- Iterator invalidation safety guarantees.

Exit criteria:

- Deterministic flush-order tests.
- Tests covering mixed create/destroy/add/remove batches.

## Phase 5: Diagnostics and Tooling

Deliverables:

- World stats API.
- Optional trace hooks for structural operations.
- Benchmark harness for throughput/latency snapshots.

Exit criteria:

- Bench output includes entity count, query throughput, and move counts.
- CI runs unit tests and benchmark smoke test.

## Phase 6: Parallel Execution (Optional v1.1)

Deliverables:

- Chunk-level parallel query execution.
- Conflict-aware scheduling for read/write queries.

Exit criteria:

- Parallel correctness tests.
- Measurable speedup on multicore benchmark scenes.

## Initial Testing Strategy

- Unit tests: entity validity, archetype transitions, query filtering.
- Property/fuzz tests: random structural operation sequences.
- Regression tests: deterministic results across repeated seeds.
- Performance tests: fixed scenes for movement, spawning, and wide queries.

