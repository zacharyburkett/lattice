# Lattice

Lattice is a high-performance Entity-Component-System (ECS) framework in C for
the Ardent engine ecosystem.

Status: early implementation. Phase 1 core runtime is in place.

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
- World stats snapshot API.
- Unit tests for entity lifecycle, growth, and component registry behavior.

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
