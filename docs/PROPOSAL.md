# Lattice Proposal

This document defines the initial direction for Lattice before implementation.

## Problem Statement

The engine needs an ECS that is:

1. Fast enough for large simulation workloads.
2. Predictable enough for deterministic frame loops.
3. Simple enough to use without deep ECS internals knowledge.

Existing ad hoc object layouts and per-system containers become hard to scale as
entity counts and feature count rise.

## Goals

- High throughput for component iteration and system updates.
- Deterministic behavior by default in single-threaded frame execution.
- Clear ownership and lifetime rules in plain C.
- Minimal runtime allocation churn in steady state.
- Good diagnostics for profiling and debugging.

## Non-Goals (Initial Milestone)

- Reflection-heavy runtime schema system.
- Hot-reload of arbitrary component layouts.
- Built-in networking replication layer.
- Full job scheduler and lock-free execution from day one.

## Options Considered

### Option A: Sparse Set ECS

Pros:
- Simple to implement and understand.
- Very fast add/remove for independent component pools.

Cons:
- Multi-component iteration causes many indirect lookups.
- Harder to sustain cache locality for wide system queries.

### Option B: Archetype-Chunk ECS

Pros:
- Excellent locality for common multi-component iteration.
- Natural fit for batched query iteration.
- Enables chunk-level scheduling and filtering later.

Cons:
- Structural changes are more complex (entity moves between archetypes).
- Higher implementation complexity than sparse set.

### Option C: Hybrid (Sparse Set + Archetype)

Pros:
- Flexible per-component storage choice.

Cons:
- More policy surface and complexity up front.
- Higher cognitive load for users early on.

## Recommended Direction

Use **Option B (Archetype-Chunk ECS)** as the default model.

Rationale:
- Best long-term performance characteristics for game-style systems.
- Keeps "easy to use" by exposing simple high-level operations while internals
  handle layout optimization.
- Leaves room to add specialized storage policies later if needed.

## Core Decisions

- Entity ID format: `index + generation` handle (stable, stale-handle detection).
- Component IDs: explicit runtime registration with size/alignment metadata.
- Storage: fixed-size chunks per archetype with SoA component columns.
- Structural edits: deferred command buffer during system execution.
- Query semantics: include/exclude component sets with read/write access modes.
- Iteration: chunk batches for contiguous processing.

## Tradeoffs

- Archetypes maximize iteration speed, but add complexity for structural changes.
- Deferred edits keep iteration stable and deterministic, but add one-frame
  latency for structural visibility.
- Explicit component registration is boilerplate, but avoids hidden magic in C.

## Success Criteria

- `create/add/remove/query/destroy` operations are functionally stable.
- Query throughput scales with entity count and component width.
- No per-frame heap churn in steady-state benchmark scenes.
- Clear error/status codes for all public API operations.
- Test coverage for stale handles, move correctness, and query correctness.

## Recommendation

Proceed with an archetype-chunk baseline plus deferred commands and a compact C
API. Keep phase 1 single-threaded and deterministic; add parallel execution only
after profiling validates the baseline.

