#ifndef LATTICE_WORLD_H
#define LATTICE_WORLD_H

#include <stddef.h>
#include <stdint.h>

#include "lattice/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lt_world_s lt_world_t;
typedef struct lt_query_s lt_query_t;

typedef void* (*lt_alloc_fn)(void* user, size_t size, size_t align);
typedef void (*lt_free_fn)(void* user, void* ptr, size_t size, size_t align);

typedef struct lt_allocator_s {
    lt_alloc_fn alloc;
    lt_free_fn free;
    void* user;
} lt_allocator_t;

enum {
    LT_COMPONENT_FLAG_NONE = 0u,
    LT_COMPONENT_FLAG_TAG = 1u << 0,
    LT_COMPONENT_FLAG_TRIVIALLY_RELOCATABLE = 1u << 1
};

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

typedef struct lt_world_config_s {
    lt_allocator_t allocator;
    uint32_t initial_entity_capacity;
    uint32_t initial_component_capacity;
    uint32_t target_chunk_bytes;
} lt_world_config_t;

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

typedef enum lt_trace_event_kind_e {
    LT_TRACE_EVENT_DEFER_BEGIN = 1,
    LT_TRACE_EVENT_DEFER_END = 2,
    LT_TRACE_EVENT_DEFER_ENQUEUE = 3,
    LT_TRACE_EVENT_FLUSH_BEGIN = 4,
    LT_TRACE_EVENT_FLUSH_APPLY = 5,
    LT_TRACE_EVENT_FLUSH_END = 6,
    LT_TRACE_EVENT_ENTITY_CREATE = 7,
    LT_TRACE_EVENT_ENTITY_DESTROY = 8,
    LT_TRACE_EVENT_COMPONENT_ADD = 9,
    LT_TRACE_EVENT_COMPONENT_REMOVE = 10,
    LT_TRACE_EVENT_QUERY_ITER_BEGIN = 11,
    LT_TRACE_EVENT_QUERY_ITER_CHUNK = 12,
    LT_TRACE_EVENT_QUERY_ITER_END = 13
} lt_trace_event_kind_t;

typedef struct lt_trace_event_s {
    lt_trace_event_kind_t kind;
    lt_status_t status;
    lt_entity_t entity;
    lt_component_id_t component_id;
    uint32_t operation;
    uint32_t live_entities;
    uint32_t pending_commands;
    uint32_t defer_depth;
} lt_trace_event_t;

typedef void (*lt_trace_hook_fn)(const lt_trace_event_t* event, void* user_data);

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

typedef void (*lt_query_parallel_chunk_fn)(
    const lt_chunk_view_t* view,
    uint32_t worker_index,
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

typedef struct lt_query_iter_s {
    lt_query_t* query;
    uint32_t archetype_index;
    void* chunk_cursor;
    void** columns;
    uint32_t column_capacity;
    uint8_t finished;
} lt_query_iter_t;

lt_status_t lt_world_create(const lt_world_config_t* cfg, lt_world_t** out_world);
void lt_world_destroy(lt_world_t* world);

lt_status_t lt_world_reserve_entities(lt_world_t* world, uint32_t entity_capacity);
lt_status_t lt_world_reserve_components(lt_world_t* world, uint32_t component_capacity);
lt_status_t lt_world_begin_defer(lt_world_t* world);
lt_status_t lt_world_end_defer(lt_world_t* world);
lt_status_t lt_world_flush(lt_world_t* world);
lt_status_t lt_world_set_trace_hook(lt_world_t* world, lt_trace_hook_fn hook, void* user_data);

lt_status_t lt_entity_create(lt_world_t* world, lt_entity_t* out_entity);
lt_status_t lt_entity_destroy(lt_world_t* world, lt_entity_t entity);
lt_status_t lt_entity_is_alive(const lt_world_t* world, lt_entity_t entity, uint8_t* out_alive);

lt_status_t lt_add_component(
    lt_world_t* world,
    lt_entity_t entity,
    lt_component_id_t component_id,
    const void* initial_value);

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
    void** out_ptr);

lt_status_t lt_register_component(
    lt_world_t* world,
    const lt_component_desc_t* desc,
    lt_component_id_t* out_id);

lt_status_t lt_world_get_stats(const lt_world_t* world, lt_world_stats_t* out_stats);

lt_status_t lt_query_create(lt_world_t* world, const lt_query_desc_t* desc, lt_query_t** out_query);
void lt_query_destroy(lt_query_t* query);
lt_status_t lt_query_refresh(lt_query_t* query);
lt_status_t lt_query_iter_begin(lt_query_t* query, lt_query_iter_t* out_iter);
lt_status_t lt_query_iter_next(
    lt_query_iter_t* iter,
    lt_chunk_view_t* out_view,
    uint8_t* out_has_value);
lt_status_t lt_query_for_each_chunk_parallel(
    lt_query_t* query,
    uint32_t worker_count,
    lt_query_parallel_chunk_fn callback,
    void* user_data);
lt_status_t lt_query_schedule_execute(
    const lt_query_schedule_entry_t* entries,
    uint32_t entry_count,
    uint32_t worker_count,
    lt_query_schedule_stats_t* out_stats);

#ifdef __cplusplus
}
#endif

#endif
