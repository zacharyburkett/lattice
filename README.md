# Lattice

Lattice is a high-performance Entity-Component-System (ECS) framework in C for
the Ardent engine ecosystem.

Status: proposal/design stage. No runtime implementation is committed yet.

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

## Docs Map

- Proposal and option analysis: `docs/PROPOSAL.md`
- System design details: `docs/ARCHITECTURE.md`
- Draft C API surface: `docs/API_SKETCH.md`
- Delivery phases and acceptance criteria: `docs/PROJECT_PLAN.md`

