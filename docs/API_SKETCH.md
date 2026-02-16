# Lattice API Sketch (Draft)

This is a proposed public C API surface. Names and signatures are draft-only.

## Design Principles

- Return status codes for all fallible operations.
- Keep ownership explicit.
- Prefer batch/chunk iteration APIs over per-entity callbacks.

## Status and Basic Types

```c
typedef enum lt_status_e {
    LT_STATUS_OK = 0,
    LT_STATUS_INVALID_ARGUMENT,
    LT_STATUS_NOT_FOUND,
    LT_STATUS_ALREADY_EXISTS,
    LT_STATUS_CAPACITY_REACHED,
    LT_STATUS_ALLOCATION_FAILED,
    LT_STATUS_STALE_ENTITY,
    LT_STATUS_CONFLICT
} lt_status_t;

typedef uint64_t lt_entity_t;
typedef uint32_t lt_component_id_t;
typedef struct lt_world_s lt_world_t;
typedef struct lt_query_s lt_query_t;
typedef struct lt_schedule_s lt_schedule_t;
```

## World Configuration

```c
typedef void* (*lt_alloc_fn)(void* user, size_t size, size_t align);
typedef void  (*lt_free_fn)(void* user, void* ptr, size_t size, size_t align);

typedef struct lt_allocator_s {
    lt_alloc_fn alloc;
    lt_free_fn free;
    void* user;
} lt_allocator_t;

typedef struct lt_world_config_s {
    lt_allocator_t allocator;
    uint32_t initial_entity_capacity;
    uint32_t initial_component_capacity;
    uint32_t target_chunk_bytes; /* e.g. 16 * 1024 */
} lt_world_config_t;

lt_status_t lt_world_create(const lt_world_config_t* cfg, lt_world_t** out_world);
void lt_world_destroy(lt_world_t* world);
```

## Component Registration

```c
typedef void (*lt_component_ctor_fn)(void* dst, uint32_t count, void* user);
typedef void (*lt_component_dtor_fn)(void* dst, uint32_t count, void* user);
typedef void (*lt_component_move_fn)(void* dst, const void* src, uint32_t count, void* user);

typedef struct lt_component_desc_s {
    const char* name;
    uint32_t size;
    uint32_t align;
    uint32_t flags;
    lt_component_ctor_fn ctor;
    lt_component_dtor_fn dtor;
    lt_component_move_fn move;
    void* user;
} lt_component_desc_t;

lt_status_t lt_register_component(
    lt_world_t* world,
    const lt_component_desc_t* desc,
    lt_component_id_t* out_id);
```

## Entity Lifecycle

```c
lt_status_t lt_entity_create(lt_world_t* world, lt_entity_t* out_entity);
lt_status_t lt_entity_destroy(lt_world_t* world, lt_entity_t entity);
lt_status_t lt_entity_is_alive(const lt_world_t* world, lt_entity_t entity, uint8_t* out_alive);
```

## Direct Component Operations

```c
lt_status_t lt_add_component(
    lt_world_t* world,
    lt_entity_t entity,
    lt_component_id_t component_id,
    const void* initial_value /* nullable for zero-init */);

lt_status_t lt_remove_component(
    lt_world_t* world,
    lt_entity_t entity,
    lt_component_id_t component_id);

lt_status_t lt_has_component(
    const lt_world_t* world,
    lt_entity_t entity,
    lt_component_id_t component_id,
    uint8_t* out_has);

lt_status_t lt_get_component(
    lt_world_t* world,
    lt_entity_t entity,
    lt_component_id_t component_id,
    void** out_ptr /* invalidated by structural changes */);
```

## Deferred Structural Operations

```c
lt_status_t lt_world_begin_defer(lt_world_t* world);
lt_status_t lt_world_end_defer(lt_world_t* world);
lt_status_t lt_world_flush(lt_world_t* world);
```

`lt_add_component`, `lt_remove_component`, and `lt_entity_destroy` queue commands
when defer mode is active.

## Query API

```c
typedef enum lt_access_e {
    LT_ACCESS_READ = 0,
    LT_ACCESS_WRITE = 1
} lt_access_t;

typedef struct lt_query_term_s {
    lt_component_id_t component_id;
    lt_access_t access;
} lt_query_term_t;

typedef struct lt_query_desc_s {
    const lt_query_term_t* with_terms;
    uint32_t with_count;
    const lt_component_id_t* without;
    uint32_t without_count;
} lt_query_desc_t;

typedef struct lt_chunk_view_s {
    uint32_t count;
    const lt_entity_t* entities;
    void** columns;
    uint32_t column_count;
} lt_chunk_view_t;

lt_status_t lt_query_create(lt_world_t* world, const lt_query_desc_t* desc, lt_query_t** out_query);
void lt_query_destroy(lt_query_t* query);
lt_status_t lt_query_refresh(lt_query_t* query); /* optional explicit cache refresh */

typedef struct lt_query_iter_s lt_query_iter_t;
lt_status_t lt_query_iter_begin(lt_query_t* query, lt_query_iter_t* out_iter);
lt_status_t lt_query_iter_next(lt_query_iter_t* iter, lt_chunk_view_t* out_view, uint8_t* out_has_value);

typedef void (*lt_query_parallel_chunk_fn)(
    const lt_chunk_view_t* view,
    uint32_t worker_index,
    void* user_data);

lt_status_t lt_query_for_each_chunk_parallel(
    lt_query_t* query,
    uint32_t worker_count,
    lt_query_parallel_chunk_fn callback,
    void* user_data);

typedef struct lt_query_schedule_entry_s {
    lt_query_t* query;
    lt_query_parallel_chunk_fn callback;
    void* user_data;
} lt_query_schedule_entry_t;

typedef struct lt_query_schedule_stats_s {
    uint32_t batch_count;
    uint32_t edge_count;
    uint32_t max_batch_size;
} lt_query_schedule_stats_t;

lt_status_t lt_schedule_create(
    const lt_query_schedule_entry_t* entries,
    uint32_t entry_count,
    lt_schedule_t** out_schedule);
void lt_schedule_destroy(lt_schedule_t* schedule);
lt_status_t lt_schedule_execute(
    lt_schedule_t* schedule,
    uint32_t worker_count,
    lt_query_schedule_stats_t* out_stats);

/* Convenience wrapper for one-shot use; steady-state loops should prefer compiled schedules. */
lt_status_t lt_query_schedule_execute(
    const lt_query_schedule_entry_t* entries,
    uint32_t entry_count,
    uint32_t worker_count,
    lt_query_schedule_stats_t* out_stats);
```

## Diagnostics

```c
typedef struct lt_world_stats_s {
    uint32_t live_entities;
    uint32_t entity_capacity;
    uint32_t allocated_entity_slots;
    uint32_t free_entity_slots;
    uint32_t registered_components;
    uint32_t archetype_count;
    uint32_t chunk_count;
    uint32_t pending_commands;
    uint32_t defer_depth;
    uint64_t structural_moves;
} lt_world_stats_t;

lt_status_t lt_world_get_stats(const lt_world_t* world, lt_world_stats_t* out_stats);
```

## Open API Decisions

- Whether typed helper macros should be first-class in v1.
- Whether defer mode should be explicit or always-on during query iteration.
- Whether command buffer should support transactional rollback in debug builds.
