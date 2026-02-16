# Lattice

Lattice is a high-performance Entity-Component-System (ECS) framework in C for
the Ardent engine ecosystem.

Status: early implementation. Phase 5 diagnostics and benchmark tooling are now in place.

## Design Intent

- Fast iteration over component data (cache-friendly, data-oriented).
- Deterministic world updates for game/simulation loops.
- Small, explicit C API with allocator-aware configuration.
- Ergonomic enough for gameplay code without hiding cost.

## Proposed Shape

- Archetype-chunk storage as the default data model.
- Stable entity handles (`index + generation`) for safety.
- Deferred structural changes (`add/remove component`, destroy) during system
  execution.
- Query API that returns contiguous batches for tight loops.

## What Exists Today

- World lifecycle API (`lt_world_create`, `lt_world_destroy`).
- Entity allocation/destruction with stale-handle protection.
- Component registration with metadata validation and duplicate-name rejection.
- Archetype/chunk storage and structural entity moves for component add/remove.
- Direct component access APIs (`lt_add_component`, `lt_remove_component`,
  `lt_has_component`, `lt_get_component`).
- Query API for archetype-matched chunk iteration (`lt_query_*`).
- Experimental chunk-level parallel query execution helper
  (`lt_query_for_each_chunk_parallel`).
- Experimental conflict-aware query scheduler with topological batches
  (`lt_query_schedule_execute`).
- Deferred structural command buffer (`lt_world_begin_defer`,
  `lt_world_end_defer`, `lt_world_flush`).
- Trace hook diagnostics for runtime event capture (`lt_world_set_trace_hook`),
  including query iteration begin/chunk/end events.
- World stats snapshot API.
- Benchmark app (`lattice_bench`) for throughput and structural-layout snapshots,
  with `--format text|csv|json` output modes, scene selection via
  `--scene steady|churn`, configurable worker sweeps via `--workers N[,N...]`
  (default `1,2,4,8`), churn tuning via `--churn-rate` and
  `--churn-initial-ratio`, speedup reporting, and scheduler batch/edge plus
  structural-operation metrics.
- Unit tests for lifecycle, registration, structural moves, query filtering,
  deferred command semantics, and trace hook coverage.

## Quick Start

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Docs Map

- Proposal and option analysis: `docs/PROPOSAL.md`
- System design details: `docs/ARCHITECTURE.md`
- Draft C API surface: `docs/API_SKETCH.md`
- Delivery phases and acceptance criteria: `docs/PROJECT_PLAN.md`
