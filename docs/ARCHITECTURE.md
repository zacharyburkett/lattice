# Lattice Architecture (Draft)

This is the proposed runtime architecture before implementation.

## Core Runtime Objects

- `lt_world_t`: owns all ECS state.
- `lt_entity_t`: opaque 64-bit handle (`index + generation`).
- `lt_component_id_t`: dense id from component registry.
- `lt_archetype_t`: unique component-set layout.
- `lt_chunk_t`: fixed-capacity storage block for one archetype.
- `lt_query_t`: compiled filter over archetypes.

## Entity Model

Entity records are stored in an index table:

- `generation`
- `location.archetype`
- `location.chunk`
- `location.row`
- free-list linkage when dead

Destroy increments generation and returns index to free list. Any stale handle
with old generation is rejected.

## Component Registration

Each component type is registered with:

- name (debug/profiling use)
- size
- alignment
- optional lifecycle hooks (construct, destruct, move/copy)
- flags (trivially relocatable, tag/zero-sized, etc.)

The registry assigns stable runtime component IDs inside a world.

## Archetype Storage

Archetype identity is a sorted component ID set. Each archetype stores:

- component column metadata (offset/stride/alignment)
- chunk list
- optional cached query match bits

Each chunk uses structure-of-arrays storage:

- one dense column per component in the archetype
- entity handle column for reverse mapping and debug
- current row count and max rows

Row capacity is derived from target chunk byte budget and component widths.

## Structural Changes

Adding/removing components moves an entity between archetypes:

1. Resolve destination archetype key.
2. Allocate destination row.
3. Move/copy shared component data.
4. Initialize added components.
5. Destruct removed components.
6. Swap-remove source row.
7. Update entity location table.

This move path is central and must be heavily tested.

## Query Engine

Query descriptor:

- include set
- exclude set
- optional required access (`read` vs `write`)

Compilation output:

- matching archetype list
- per-archetype column lookup table

Iteration model:

- iterate chunk-by-chunk
- return contiguous column pointers + row count

This keeps inner loops branch-light and cache-friendly.

## Deferred Command Buffer

During system execution, structural changes are queued:

- create/destroy entity
- add/remove component
- set component (optional convenience)

At sync points (`lt_world_flush`), queued commands are applied in order. This
prevents iterator invalidation and keeps phase execution deterministic.

## Scheduling Model (Initial)

Phase 1:

- User drives explicit update order.
- Queries execute on the calling thread.
- No hidden parallelism.

Phase 2+:

- Chunk-level parallel execution for non-conflicting queries.
- Conflict detection from query access modes.

## Memory Strategy

- World-level allocator hooks.
- Chunk pools for stable reuse.
- No implicit global allocators.
- Optional reserve calls to pre-size entity table/archetype pools.

## Debug and Instrumentation

Expose counters and snapshots:

- live entities
- archetype count
- chunk usage
- command-buffer length
- total moves caused by structural changes

These should be queryable at runtime for profiling tools.

## Complexity Targets

- Entity create/destroy: amortized O(1)
- Add/remove component: O(k) where k is moved component count
- Query compile: O(archetype_count)
- Query iterate: O(matching_entities)

