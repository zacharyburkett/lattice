#include "lattice/world.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifndef __STDC_VERSION__
#error "C11 or newer is required"
#endif

enum {
    LT_DEFAULT_CHUNK_BYTES = 16u * 1024u,
    LT_MAX_ROWS_PER_CHUNK = 4096u
};

typedef enum lt_deferred_op_kind_e {
    LT_DEFERRED_OP_ADD_COMPONENT = 1,
    LT_DEFERRED_OP_REMOVE_COMPONENT = 2,
    LT_DEFERRED_OP_DESTROY_ENTITY = 3
} lt_deferred_op_kind_t;

typedef struct lt_archetype_s lt_archetype_t;
typedef struct lt_chunk_s lt_chunk_t;

typedef struct lt_entity_slot_s {
    uint32_t generation;
    uint32_t next_free;
    uint8_t alive;
    lt_archetype_t* archetype;
    lt_chunk_t* chunk;
    uint32_t row;
} lt_entity_slot_t;

typedef struct lt_component_record_s {
    char* name;
    uint32_t size;
    uint32_t align;
    uint32_t flags;
    lt_component_ctor_fn ctor;
    lt_component_dtor_fn dtor;
    lt_component_move_fn move;
    void* user;
} lt_component_record_t;

typedef struct lt_deferred_op_s {
    lt_deferred_op_kind_t kind;
    lt_entity_t entity;
    lt_component_id_t component_id;
    void* payload;
    uint32_t payload_size;
    uint32_t payload_align;
} lt_deferred_op_t;

struct lt_chunk_s {
    lt_chunk_t* next;
    uint32_t count;
    uint32_t capacity;
    lt_entity_t* entities;
    uint8_t** columns;
};

struct lt_archetype_s {
    lt_component_id_t* component_ids;
    uint32_t component_count;
    uint32_t rows_per_chunk;
    lt_chunk_t* chunks;
    lt_chunk_t* chunk_tail;
    uint32_t chunk_count;
};

struct lt_world_s {
    lt_allocator_t allocator;
    uint32_t target_chunk_bytes;
    lt_trace_hook_fn trace_hook;
    void* trace_user_data;

    lt_entity_slot_t* entities;
    uint32_t entity_capacity;
    uint32_t entity_count;
    uint32_t live_entity_count;
    uint32_t free_entity_count;
    uint32_t free_entity_head;

    lt_component_record_t* components;
    uint32_t component_capacity;
    uint32_t component_count;

    lt_archetype_t** archetypes;
    uint32_t archetype_capacity;
    uint32_t archetype_count;
    uint32_t total_chunk_count;
    lt_archetype_t* root_archetype;

    lt_deferred_op_t* deferred_ops;
    uint32_t deferred_count;
    uint32_t deferred_capacity;
    uint32_t defer_depth;
    uint64_t structural_move_count;
};

struct lt_query_s {
    lt_world_t* world;
    lt_query_term_t* with_terms;
    uint32_t with_count;
    lt_component_id_t* without;
    uint32_t without_count;
    lt_archetype_t** matches;
    uint32_t match_count;
    uint32_t match_capacity;
    void** scratch_columns;
    uint32_t scratch_capacity;
};

void lt_query_destroy(lt_query_t* query);

static int lt_is_power_of_two_u32(uint32_t v)
{
    return v != 0u && (v & (v - 1u)) == 0u;
}

static lt_entity_t lt_entity_pack(uint32_t index, uint32_t generation)
{
    return ((uint64_t)generation << 32u) | (uint64_t)index;
}

static uint32_t lt_entity_index(lt_entity_t entity)
{
    return (uint32_t)(entity & 0xFFFFFFFFu);
}

static uint32_t lt_entity_generation(lt_entity_t entity)
{
    return (uint32_t)(entity >> 32u);
}

static void* lt_default_alloc(void* user, size_t size, size_t align)
{
    void* ptr;

    (void)user;

    if (size == 0u) {
        return NULL;
    }

    if (align <= sizeof(void*)) {
        return malloc(size);
    }

#if defined(_MSC_VER)
    ptr = _aligned_malloc(size, align);
    return ptr;
#else
    if (posix_memalign(&ptr, align, size) != 0) {
        return NULL;
    }
    return ptr;
#endif
}

static void lt_default_free(void* user, void* ptr, size_t size, size_t align)
{
    (void)user;
    (void)size;
    (void)align;

#if defined(_MSC_VER)
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

static void lt_init_allocator(lt_allocator_t* allocator)
{
    if (allocator == NULL) {
        return;
    }

    if (allocator->alloc == NULL) {
        allocator->alloc = lt_default_alloc;
    }
    if (allocator->free == NULL) {
        allocator->free = lt_default_free;
    }
}

static lt_status_t lt_prepare_allocator(
    const lt_world_config_t* cfg,
    lt_allocator_t* out_allocator)
{
    if (out_allocator == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    memset(out_allocator, 0, sizeof(*out_allocator));
    if (cfg != NULL) {
        *out_allocator = cfg->allocator;
    }

    if ((out_allocator->alloc == NULL) != (out_allocator->free == NULL)) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    lt_init_allocator(out_allocator);
    return LT_STATUS_OK;
}

static void* lt_alloc_bytes(lt_allocator_t* allocator, size_t size, size_t align)
{
    if (allocator == NULL || allocator->alloc == NULL || size == 0u) {
        return NULL;
    }
    return allocator->alloc(allocator->user, size, align);
}

static void lt_free_bytes(lt_allocator_t* allocator, void* ptr, size_t size, size_t align)
{
    if (allocator == NULL || allocator->free == NULL || ptr == NULL) {
        return;
    }
    allocator->free(allocator->user, ptr, size, align);
}

static void lt_trace_emit(
    lt_world_t* world,
    lt_trace_event_kind_t kind,
    lt_status_t status,
    lt_entity_t entity,
    lt_component_id_t component_id,
    uint32_t operation)
{
    lt_trace_event_t event;

    if (world == NULL || world->trace_hook == NULL) {
        return;
    }

    memset(&event, 0, sizeof(event));
    event.kind = kind;
    event.status = status;
    event.entity = entity;
    event.component_id = component_id;
    event.operation = operation;
    event.live_entities = world->live_entity_count;
    event.pending_commands = world->deferred_count;
    event.defer_depth = world->defer_depth;
    world->trace_hook(&event, world->trace_user_data);
}

static void lt_deferred_op_release(lt_world_t* world, lt_deferred_op_t* op)
{
    if (world == NULL || op == NULL) {
        return;
    }

    if (op->payload != NULL) {
        lt_free_bytes(
            &world->allocator,
            op->payload,
            (size_t)op->payload_size,
            op->payload_align == 0u ? _Alignof(max_align_t) : (size_t)op->payload_align);
    }

    memset(op, 0, sizeof(*op));
}

static void lt_deferred_clear(lt_world_t* world)
{
    uint32_t i;

    if (world == NULL || world->deferred_ops == NULL || world->deferred_count == 0u) {
        if (world != NULL) {
            world->deferred_count = 0u;
        }
        return;
    }

    for (i = 0u; i < world->deferred_count; ++i) {
        lt_deferred_op_release(world, &world->deferred_ops[i]);
    }
    world->deferred_count = 0u;
}

static lt_status_t lt_deferred_grow(lt_world_t* world, uint32_t min_capacity)
{
    uint32_t old_capacity;
    uint32_t new_capacity;
    size_t old_size;
    size_t new_size;
    lt_deferred_op_t* new_ops;

    if (world == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    if (world->deferred_capacity >= min_capacity) {
        return LT_STATUS_OK;
    }

    old_capacity = world->deferred_capacity;
    new_capacity = old_capacity == 0u ? 64u : old_capacity;
    while (new_capacity < min_capacity) {
        if (new_capacity > UINT32_MAX / 2u) {
            return LT_STATUS_CAPACITY_REACHED;
        }
        new_capacity *= 2u;
    }

    if (sizeof(*new_ops) > SIZE_MAX / (size_t)new_capacity) {
        return LT_STATUS_CAPACITY_REACHED;
    }

    new_size = sizeof(*new_ops) * (size_t)new_capacity;
    new_ops = (lt_deferred_op_t*)lt_alloc_bytes(
        &world->allocator,
        new_size,
        _Alignof(lt_deferred_op_t));
    if (new_ops == NULL) {
        return LT_STATUS_ALLOCATION_FAILED;
    }
    memset(new_ops, 0, new_size);

    if (world->deferred_ops != NULL && world->deferred_count > 0u) {
        old_size = sizeof(*new_ops) * (size_t)world->deferred_count;
        memcpy(new_ops, world->deferred_ops, old_size);
    }

    if (world->deferred_ops != NULL) {
        lt_free_bytes(
            &world->allocator,
            world->deferred_ops,
            sizeof(*world->deferred_ops) * (size_t)old_capacity,
            _Alignof(lt_deferred_op_t));
    }

    world->deferred_ops = new_ops;
    world->deferred_capacity = new_capacity;
    return LT_STATUS_OK;
}

static lt_status_t lt_enqueue_destroy_entity(lt_world_t* world, lt_entity_t entity)
{
    lt_deferred_op_t* op;
    lt_status_t status;

    if (world == NULL || entity == LT_ENTITY_NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    status = lt_deferred_grow(world, world->deferred_count + 1u);
    if (status != LT_STATUS_OK) {
        lt_trace_emit(
            world,
            LT_TRACE_EVENT_DEFER_ENQUEUE,
            status,
            entity,
            LT_COMPONENT_INVALID,
            (uint32_t)LT_DEFERRED_OP_DESTROY_ENTITY);
        return status;
    }

    op = &world->deferred_ops[world->deferred_count];
    memset(op, 0, sizeof(*op));
    op->kind = LT_DEFERRED_OP_DESTROY_ENTITY;
    op->entity = entity;
    world->deferred_count += 1u;
    lt_trace_emit(
        world,
        LT_TRACE_EVENT_DEFER_ENQUEUE,
        LT_STATUS_OK,
        entity,
        LT_COMPONENT_INVALID,
        (uint32_t)LT_DEFERRED_OP_DESTROY_ENTITY);
    return LT_STATUS_OK;
}

static lt_status_t lt_enqueue_remove_component(
    lt_world_t* world,
    lt_entity_t entity,
    lt_component_id_t component_id)
{
    lt_deferred_op_t* op;
    lt_status_t status;

    if (world == NULL || entity == LT_ENTITY_NULL || component_id == LT_COMPONENT_INVALID) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    status = lt_deferred_grow(world, world->deferred_count + 1u);
    if (status != LT_STATUS_OK) {
        lt_trace_emit(
            world,
            LT_TRACE_EVENT_DEFER_ENQUEUE,
            status,
            entity,
            component_id,
            (uint32_t)LT_DEFERRED_OP_REMOVE_COMPONENT);
        return status;
    }

    op = &world->deferred_ops[world->deferred_count];
    memset(op, 0, sizeof(*op));
    op->kind = LT_DEFERRED_OP_REMOVE_COMPONENT;
    op->entity = entity;
    op->component_id = component_id;
    world->deferred_count += 1u;
    lt_trace_emit(
        world,
        LT_TRACE_EVENT_DEFER_ENQUEUE,
        LT_STATUS_OK,
        entity,
        component_id,
        (uint32_t)LT_DEFERRED_OP_REMOVE_COMPONENT);
    return LT_STATUS_OK;
}

static lt_status_t lt_enqueue_add_component(
    lt_world_t* world,
    lt_entity_t entity,
    lt_component_id_t component_id,
    const void* initial_value)
{
    const lt_component_record_t* component;
    lt_deferred_op_t* op;
    lt_status_t status;

    if (world == NULL || entity == LT_ENTITY_NULL || component_id == LT_COMPONENT_INVALID) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    component = &world->components[component_id];

    status = lt_deferred_grow(world, world->deferred_count + 1u);
    if (status != LT_STATUS_OK) {
        lt_trace_emit(
            world,
            LT_TRACE_EVENT_DEFER_ENQUEUE,
            status,
            entity,
            component_id,
            (uint32_t)LT_DEFERRED_OP_ADD_COMPONENT);
        return status;
    }

    op = &world->deferred_ops[world->deferred_count];
    memset(op, 0, sizeof(*op));
    op->kind = LT_DEFERRED_OP_ADD_COMPONENT;
    op->entity = entity;
    op->component_id = component_id;

    if (component->size > 0u && initial_value != NULL) {
        op->payload = lt_alloc_bytes(&world->allocator, component->size, component->align);
        if (op->payload == NULL) {
            lt_trace_emit(
                world,
                LT_TRACE_EVENT_DEFER_ENQUEUE,
                LT_STATUS_ALLOCATION_FAILED,
                entity,
                component_id,
                (uint32_t)LT_DEFERRED_OP_ADD_COMPONENT);
            return LT_STATUS_ALLOCATION_FAILED;
        }
        memcpy(op->payload, initial_value, component->size);
        op->payload_size = component->size;
        op->payload_align = component->align;
    }

    world->deferred_count += 1u;
    lt_trace_emit(
        world,
        LT_TRACE_EVENT_DEFER_ENQUEUE,
        LT_STATUS_OK,
        entity,
        component_id,
        (uint32_t)LT_DEFERRED_OP_ADD_COMPONENT);
    return LT_STATUS_OK;
}

static lt_status_t lt_grow_entities(lt_world_t* world, uint32_t min_capacity)
{
    uint32_t old_capacity;
    uint32_t new_capacity;
    size_t old_size;
    size_t new_size;
    lt_entity_slot_t* new_entities;

    if (world == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    if (world->entity_capacity >= min_capacity) {
        return LT_STATUS_OK;
    }

    old_capacity = world->entity_capacity;
    new_capacity = old_capacity == 0u ? 64u : old_capacity;
    while (new_capacity < min_capacity) {
        if (new_capacity > UINT32_MAX / 2u) {
            return LT_STATUS_CAPACITY_REACHED;
        }
        new_capacity *= 2u;
    }

    if (sizeof(*new_entities) > SIZE_MAX / new_capacity) {
        return LT_STATUS_CAPACITY_REACHED;
    }

    new_size = sizeof(*new_entities) * (size_t)new_capacity;
    new_entities = (lt_entity_slot_t*)lt_alloc_bytes(
        &world->allocator,
        new_size,
        _Alignof(lt_entity_slot_t));
    if (new_entities == NULL) {
        return LT_STATUS_ALLOCATION_FAILED;
    }
    memset(new_entities, 0, new_size);

    if (world->entities != NULL && old_capacity > 0u) {
        old_size = sizeof(*new_entities) * (size_t)old_capacity;
        memcpy(new_entities, world->entities, old_size);
        lt_free_bytes(
            &world->allocator,
            world->entities,
            old_size,
            _Alignof(lt_entity_slot_t));
    }

    world->entities = new_entities;
    world->entity_capacity = new_capacity;
    return LT_STATUS_OK;
}

static lt_status_t lt_grow_components(lt_world_t* world, uint32_t min_capacity)
{
    uint32_t old_capacity;
    uint32_t new_capacity;
    size_t old_size;
    size_t new_size;
    lt_component_record_t* new_components;

    if (world == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    if (world->component_capacity >= min_capacity) {
        return LT_STATUS_OK;
    }

    old_capacity = world->component_capacity;
    new_capacity = old_capacity == 0u ? 16u : old_capacity;
    while (new_capacity < min_capacity) {
        if (new_capacity > UINT32_MAX / 2u) {
            return LT_STATUS_CAPACITY_REACHED;
        }
        new_capacity *= 2u;
    }

    if (sizeof(*new_components) > SIZE_MAX / (size_t)(new_capacity + 1u)) {
        return LT_STATUS_CAPACITY_REACHED;
    }

    new_size = sizeof(*new_components) * (size_t)(new_capacity + 1u);
    new_components = (lt_component_record_t*)lt_alloc_bytes(
        &world->allocator,
        new_size,
        _Alignof(lt_component_record_t));
    if (new_components == NULL) {
        return LT_STATUS_ALLOCATION_FAILED;
    }
    memset(new_components, 0, new_size);

    if (world->components != NULL && old_capacity > 0u) {
        old_size = sizeof(*new_components) * (size_t)(old_capacity + 1u);
        memcpy(new_components, world->components, old_size);
        lt_free_bytes(
            &world->allocator,
            world->components,
            old_size,
            _Alignof(lt_component_record_t));
    }

    world->components = new_components;
    world->component_capacity = new_capacity;
    return LT_STATUS_OK;
}

static lt_status_t lt_grow_archetypes(lt_world_t* world, uint32_t min_capacity)
{
    uint32_t old_capacity;
    uint32_t new_capacity;
    size_t old_size;
    size_t new_size;
    lt_archetype_t** new_archetypes;

    if (world == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    if (world->archetype_capacity >= min_capacity) {
        return LT_STATUS_OK;
    }

    old_capacity = world->archetype_capacity;
    new_capacity = old_capacity == 0u ? 8u : old_capacity;
    while (new_capacity < min_capacity) {
        if (new_capacity > UINT32_MAX / 2u) {
            return LT_STATUS_CAPACITY_REACHED;
        }
        new_capacity *= 2u;
    }

    if (sizeof(*new_archetypes) > SIZE_MAX / new_capacity) {
        return LT_STATUS_CAPACITY_REACHED;
    }

    new_size = sizeof(*new_archetypes) * (size_t)new_capacity;
    new_archetypes = (lt_archetype_t**)lt_alloc_bytes(
        &world->allocator,
        new_size,
        _Alignof(lt_archetype_t*));
    if (new_archetypes == NULL) {
        return LT_STATUS_ALLOCATION_FAILED;
    }
    memset(new_archetypes, 0, new_size);

    if (world->archetypes != NULL && old_capacity > 0u) {
        old_size = sizeof(*new_archetypes) * (size_t)old_capacity;
        memcpy(new_archetypes, world->archetypes, old_size);
        lt_free_bytes(
            &world->allocator,
            world->archetypes,
            old_size,
            _Alignof(lt_archetype_t*));
    }

    world->archetypes = new_archetypes;
    world->archetype_capacity = new_capacity;
    return LT_STATUS_OK;
}

static int lt_component_names_equal(const char* a, const char* b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }
    return strcmp(a, b) == 0;
}

static lt_status_t lt_component_desc_validate(const lt_component_desc_t* desc)
{
    if (desc == NULL || desc->name == NULL || desc->name[0] == '\0') {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    if ((desc->flags & LT_COMPONENT_FLAG_TAG) != 0u) {
        if (desc->size != 0u) {
            return LT_STATUS_INVALID_ARGUMENT;
        }
        if (desc->align != 0u && desc->align != 1u) {
            return LT_STATUS_INVALID_ARGUMENT;
        }
        return LT_STATUS_OK;
    }

    if (desc->size == 0u) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    if (!lt_is_power_of_two_u32(desc->align)) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    return LT_STATUS_OK;
}

static lt_status_t lt_component_name_copy(
    lt_world_t* world,
    const char* src,
    char** out_name)
{
    size_t length;
    char* dst;

    if (world == NULL || src == NULL || out_name == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    *out_name = NULL;
    length = strlen(src) + 1u;
    dst = (char*)lt_alloc_bytes(&world->allocator, length, _Alignof(char));
    if (dst == NULL) {
        return LT_STATUS_ALLOCATION_FAILED;
    }

    memcpy(dst, src, length);
    *out_name = dst;
    return LT_STATUS_OK;
}

static uint32_t lt_compute_rows_per_chunk(
    const lt_world_t* world,
    const lt_component_id_t* component_ids,
    uint32_t component_count)
{
    size_t per_row_bytes;
    uint32_t i;
    uint32_t rows;

    per_row_bytes = sizeof(lt_entity_t);
    for (i = 0u; i < component_count; ++i) {
        const lt_component_record_t* component;
        lt_component_id_t component_id;

        component_id = component_ids[i];
        component = &world->components[component_id];
        if (component->size > SIZE_MAX - per_row_bytes) {
            return 1u;
        }
        per_row_bytes += component->size;
    }

    if (per_row_bytes == 0u) {
        return 1u;
    }

    rows = (uint32_t)((size_t)world->target_chunk_bytes / per_row_bytes);
    if (rows == 0u) {
        rows = 1u;
    }
    if (rows > LT_MAX_ROWS_PER_CHUNK) {
        rows = LT_MAX_ROWS_PER_CHUNK;
    }
    return rows;
}

static int lt_component_id_set_equal(
    const lt_component_id_t* a,
    uint32_t a_count,
    const lt_component_id_t* b,
    uint32_t b_count)
{
    if (a_count != b_count) {
        return 0;
    }
    if (a_count == 0u) {
        return 1;
    }
    return memcmp(a, b, sizeof(*a) * (size_t)a_count) == 0;
}

static lt_archetype_t* lt_find_archetype(
    const lt_world_t* world,
    const lt_component_id_t* component_ids,
    uint32_t component_count)
{
    uint32_t i;

    for (i = 0u; i < world->archetype_count; ++i) {
        lt_archetype_t* archetype;

        archetype = world->archetypes[i];
        if (archetype != NULL
            && lt_component_id_set_equal(
                archetype->component_ids,
                archetype->component_count,
                component_ids,
                component_count)) {
            return archetype;
        }
    }

    return NULL;
}

static lt_status_t lt_archetype_create(
    lt_world_t* world,
    const lt_component_id_t* component_ids,
    uint32_t component_count,
    lt_archetype_t** out_archetype)
{
    lt_archetype_t* archetype;
    lt_status_t status;

    if (world == NULL || out_archetype == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    *out_archetype = NULL;

    if (world->archetype_count == UINT32_MAX) {
        return LT_STATUS_CAPACITY_REACHED;
    }

    status = lt_grow_archetypes(world, world->archetype_count + 1u);
    if (status != LT_STATUS_OK) {
        return status;
    }

    archetype = (lt_archetype_t*)lt_alloc_bytes(
        &world->allocator,
        sizeof(*archetype),
        _Alignof(lt_archetype_t));
    if (archetype == NULL) {
        return LT_STATUS_ALLOCATION_FAILED;
    }
    memset(archetype, 0, sizeof(*archetype));

    if (component_count > 0u) {
        size_t ids_size;

        if (sizeof(*component_ids) > SIZE_MAX / component_count) {
            lt_free_bytes(&world->allocator, archetype, sizeof(*archetype), _Alignof(lt_archetype_t));
            return LT_STATUS_CAPACITY_REACHED;
        }

        ids_size = sizeof(*component_ids) * (size_t)component_count;
        archetype->component_ids = (lt_component_id_t*)lt_alloc_bytes(
            &world->allocator,
            ids_size,
            _Alignof(lt_component_id_t));
        if (archetype->component_ids == NULL) {
            lt_free_bytes(&world->allocator, archetype, sizeof(*archetype), _Alignof(lt_archetype_t));
            return LT_STATUS_ALLOCATION_FAILED;
        }
        memcpy(archetype->component_ids, component_ids, ids_size);
    }

    archetype->component_count = component_count;
    archetype->rows_per_chunk = lt_compute_rows_per_chunk(world, component_ids, component_count);
    if (archetype->rows_per_chunk == 0u) {
        archetype->rows_per_chunk = 1u;
    }

    world->archetypes[world->archetype_count] = archetype;
    world->archetype_count += 1u;

    *out_archetype = archetype;
    return LT_STATUS_OK;
}

static lt_status_t lt_find_or_create_archetype(
    lt_world_t* world,
    const lt_component_id_t* component_ids,
    uint32_t component_count,
    lt_archetype_t** out_archetype)
{
    lt_archetype_t* archetype;

    if (world == NULL || out_archetype == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    archetype = lt_find_archetype(world, component_ids, component_count);
    if (archetype != NULL) {
        *out_archetype = archetype;
        return LT_STATUS_OK;
    }

    return lt_archetype_create(world, component_ids, component_count, out_archetype);
}

static int lt_archetype_find_component_index(
    const lt_archetype_t* archetype,
    lt_component_id_t component_id,
    uint32_t* out_index)
{
    uint32_t i;

    if (archetype == NULL || component_id == LT_COMPONENT_INVALID) {
        return 0;
    }

    for (i = 0u; i < archetype->component_count; ++i) {
        if (archetype->component_ids[i] == component_id) {
            if (out_index != NULL) {
                *out_index = i;
            }
            return 1;
        }
    }

    return 0;
}

static void* lt_chunk_component_ptr(
    const lt_world_t* world,
    const lt_archetype_t* archetype,
    lt_chunk_t* chunk,
    uint32_t row,
    uint32_t component_index)
{
    lt_component_id_t component_id;
    const lt_component_record_t* component;

    component_id = archetype->component_ids[component_index];
    component = &world->components[component_id];

    if (component->size == 0u) {
        return NULL;
    }

    return (void*)(chunk->columns[component_index] + (size_t)component->size * (size_t)row);
}

static void lt_component_transfer(
    const lt_component_record_t* component,
    void* dst,
    const void* src)
{
    if (component == NULL || component->size == 0u || dst == NULL || src == NULL || dst == src) {
        return;
    }

    if (component->move != NULL) {
        component->move(dst, src, 1u, component->user);
    } else {
        memcpy(dst, src, component->size);
    }
}

static void lt_component_init_added(
    const lt_component_record_t* component,
    void* dst,
    const void* initial_value)
{
    if (component == NULL || component->size == 0u || dst == NULL) {
        return;
    }

    if (initial_value != NULL) {
        memcpy(dst, initial_value, component->size);
    } else if (component->ctor != NULL) {
        component->ctor(dst, 1u, component->user);
    } else {
        memset(dst, 0, component->size);
    }
}

static void lt_component_destruct_one(
    const lt_component_record_t* component,
    void* dst)
{
    if (component == NULL || component->size == 0u || dst == NULL || component->dtor == NULL) {
        return;
    }

    component->dtor(dst, 1u, component->user);
}

static lt_status_t lt_chunk_create(
    lt_world_t* world,
    lt_archetype_t* archetype,
    lt_chunk_t** out_chunk)
{
    lt_chunk_t* chunk;
    uint32_t i;

    if (world == NULL || archetype == NULL || out_chunk == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    *out_chunk = NULL;

    chunk = (lt_chunk_t*)lt_alloc_bytes(&world->allocator, sizeof(*chunk), _Alignof(lt_chunk_t));
    if (chunk == NULL) {
        return LT_STATUS_ALLOCATION_FAILED;
    }
    memset(chunk, 0, sizeof(*chunk));

    chunk->capacity = archetype->rows_per_chunk;
    if (chunk->capacity == 0u) {
        chunk->capacity = 1u;
    }

    if (sizeof(*chunk->entities) > SIZE_MAX / chunk->capacity) {
        lt_free_bytes(&world->allocator, chunk, sizeof(*chunk), _Alignof(lt_chunk_t));
        return LT_STATUS_CAPACITY_REACHED;
    }

    chunk->entities = (lt_entity_t*)lt_alloc_bytes(
        &world->allocator,
        sizeof(*chunk->entities) * (size_t)chunk->capacity,
        _Alignof(lt_entity_t));
    if (chunk->entities == NULL) {
        lt_free_bytes(&world->allocator, chunk, sizeof(*chunk), _Alignof(lt_chunk_t));
        return LT_STATUS_ALLOCATION_FAILED;
    }
    memset(chunk->entities, 0, sizeof(*chunk->entities) * (size_t)chunk->capacity);

    if (archetype->component_count > 0u) {
        if (sizeof(*chunk->columns) > SIZE_MAX / archetype->component_count) {
            lt_free_bytes(
                &world->allocator,
                chunk->entities,
                sizeof(*chunk->entities) * (size_t)chunk->capacity,
                _Alignof(lt_entity_t));
            lt_free_bytes(&world->allocator, chunk, sizeof(*chunk), _Alignof(lt_chunk_t));
            return LT_STATUS_CAPACITY_REACHED;
        }

        chunk->columns = (uint8_t**)lt_alloc_bytes(
            &world->allocator,
            sizeof(*chunk->columns) * (size_t)archetype->component_count,
            _Alignof(uint8_t*));
        if (chunk->columns == NULL) {
            lt_free_bytes(
                &world->allocator,
                chunk->entities,
                sizeof(*chunk->entities) * (size_t)chunk->capacity,
                _Alignof(lt_entity_t));
            lt_free_bytes(&world->allocator, chunk, sizeof(*chunk), _Alignof(lt_chunk_t));
            return LT_STATUS_ALLOCATION_FAILED;
        }
        memset(chunk->columns, 0, sizeof(*chunk->columns) * (size_t)archetype->component_count);

        for (i = 0u; i < archetype->component_count; ++i) {
            lt_component_id_t component_id;
            const lt_component_record_t* component;
            size_t column_size;

            component_id = archetype->component_ids[i];
            component = &world->components[component_id];
            if (component->size == 0u) {
                continue;
            }

            if (component->size > SIZE_MAX / chunk->capacity) {
                break;
            }

            column_size = (size_t)component->size * (size_t)chunk->capacity;
            chunk->columns[i] = (uint8_t*)lt_alloc_bytes(
                &world->allocator,
                column_size,
                component->align);
            if (chunk->columns[i] == NULL) {
                break;
            }
            memset(chunk->columns[i], 0, column_size);
        }

        if (i != archetype->component_count) {
            uint32_t j;
            for (j = 0u; j < i; ++j) {
                lt_component_id_t component_id;
                const lt_component_record_t* component;

                component_id = archetype->component_ids[j];
                component = &world->components[component_id];
                if (component->size > 0u) {
                    lt_free_bytes(
                        &world->allocator,
                        chunk->columns[j],
                        (size_t)component->size * (size_t)chunk->capacity,
                        component->align);
                }
            }

            lt_free_bytes(
                &world->allocator,
                chunk->columns,
                sizeof(*chunk->columns) * (size_t)archetype->component_count,
                _Alignof(uint8_t*));
            lt_free_bytes(
                &world->allocator,
                chunk->entities,
                sizeof(*chunk->entities) * (size_t)chunk->capacity,
                _Alignof(lt_entity_t));
            lt_free_bytes(&world->allocator, chunk, sizeof(*chunk), _Alignof(lt_chunk_t));
            return LT_STATUS_ALLOCATION_FAILED;
        }
    }

    *out_chunk = chunk;
    return LT_STATUS_OK;
}

static void lt_chunk_destroy(
    lt_world_t* world,
    const lt_archetype_t* archetype,
    lt_chunk_t* chunk,
    int destroy_live_rows)
{
    uint32_t i;

    if (world == NULL || archetype == NULL || chunk == NULL) {
        return;
    }

    if (destroy_live_rows != 0u && chunk->count > 0u) {
        uint32_t row;
        for (row = 0u; row < chunk->count; ++row) {
            for (i = 0u; i < archetype->component_count; ++i) {
                lt_component_id_t component_id;
                const lt_component_record_t* component;
                void* ptr;

                component_id = archetype->component_ids[i];
                component = &world->components[component_id];
                if (component->dtor == NULL || component->size == 0u) {
                    continue;
                }

                ptr = lt_chunk_component_ptr(world, archetype, chunk, row, i);
                lt_component_destruct_one(component, ptr);
            }
        }
    }

    if (chunk->columns != NULL) {
        for (i = 0u; i < archetype->component_count; ++i) {
            lt_component_id_t component_id;
            const lt_component_record_t* component;

            component_id = archetype->component_ids[i];
            component = &world->components[component_id];
            if (component->size > 0u && chunk->columns[i] != NULL) {
                lt_free_bytes(
                    &world->allocator,
                    chunk->columns[i],
                    (size_t)component->size * (size_t)chunk->capacity,
                    component->align);
            }
        }

        lt_free_bytes(
            &world->allocator,
            chunk->columns,
            sizeof(*chunk->columns) * (size_t)archetype->component_count,
            _Alignof(uint8_t*));
    }

    if (chunk->entities != NULL) {
        lt_free_bytes(
            &world->allocator,
            chunk->entities,
            sizeof(*chunk->entities) * (size_t)chunk->capacity,
            _Alignof(lt_entity_t));
    }

    lt_free_bytes(&world->allocator, chunk, sizeof(*chunk), _Alignof(lt_chunk_t));
}

static lt_status_t lt_archetype_alloc_row(
    lt_world_t* world,
    lt_archetype_t* archetype,
    lt_chunk_t** out_chunk,
    uint32_t* out_row)
{
    lt_chunk_t* chunk;

    if (world == NULL || archetype == NULL || out_chunk == NULL || out_row == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    chunk = archetype->chunks;
    while (chunk != NULL) {
        if (chunk->count < chunk->capacity) {
            *out_chunk = chunk;
            *out_row = chunk->count;
            chunk->count += 1u;
            return LT_STATUS_OK;
        }
        chunk = chunk->next;
    }

    {
        lt_status_t status;
        status = lt_chunk_create(world, archetype, &chunk);
        if (status != LT_STATUS_OK) {
            return status;
        }
    }

    if (archetype->chunk_tail != NULL) {
        archetype->chunk_tail->next = chunk;
    } else {
        archetype->chunks = chunk;
    }
    archetype->chunk_tail = chunk;
    archetype->chunk_count += 1u;
    world->total_chunk_count += 1u;

    *out_chunk = chunk;
    *out_row = 0u;
    chunk->count = 1u;
    return LT_STATUS_OK;
}

static void lt_archetype_swap_remove_row(
    lt_world_t* world,
    lt_archetype_t* archetype,
    lt_chunk_t* chunk,
    uint32_t row)
{
    uint32_t last_row;
    uint32_t i;

    if (world == NULL || archetype == NULL || chunk == NULL || chunk->count == 0u || row >= chunk->count) {
        return;
    }

    last_row = chunk->count - 1u;
    if (row != last_row) {
        lt_entity_t moved_entity;

        world->structural_move_count += 1u;
        moved_entity = chunk->entities[last_row];
        chunk->entities[row] = moved_entity;

        for (i = 0u; i < archetype->component_count; ++i) {
            lt_component_id_t component_id;
            const lt_component_record_t* component;
            void* dst_ptr;
            void* src_ptr;

            component_id = archetype->component_ids[i];
            component = &world->components[component_id];
            if (component->size == 0u) {
                continue;
            }

            dst_ptr = lt_chunk_component_ptr(world, archetype, chunk, row, i);
            src_ptr = lt_chunk_component_ptr(world, archetype, chunk, last_row, i);
            lt_component_transfer(component, dst_ptr, src_ptr);
        }

        {
            uint32_t moved_index;
            lt_entity_slot_t* moved_slot;

            moved_index = lt_entity_index(moved_entity);
            if (moved_index < world->entity_count) {
                moved_slot = &world->entities[moved_index];
                moved_slot->archetype = archetype;
                moved_slot->chunk = chunk;
                moved_slot->row = row;
            }
        }
    }

    chunk->count -= 1u;
}

static lt_status_t lt_world_get_live_slot(
    const lt_world_t* world,
    lt_entity_t entity,
    lt_entity_slot_t** out_slot)
{
    uint32_t index;
    uint32_t generation;
    lt_entity_slot_t* slot;

    if (world == NULL || out_slot == NULL || entity == LT_ENTITY_NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    index = lt_entity_index(entity);
    generation = lt_entity_generation(entity);
    if (index >= world->entity_count) {
        return LT_STATUS_STALE_ENTITY;
    }

    slot = &((lt_world_t*)world)->entities[index];
    if (slot->alive == 0u || slot->generation != generation) {
        return LT_STATUS_STALE_ENTITY;
    }

    *out_slot = slot;
    return LT_STATUS_OK;
}

static lt_status_t lt_world_component_key_with_add(
    lt_world_t* world,
    const lt_archetype_t* archetype,
    lt_component_id_t add_id,
    lt_component_id_t** out_ids,
    uint32_t* out_count)
{
    lt_component_id_t* ids;
    uint32_t i;
    uint32_t src_i;
    uint32_t dst_i;
    uint32_t count;

    if (world == NULL || archetype == NULL || out_ids == NULL || out_count == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    count = archetype->component_count + 1u;
    if (sizeof(*ids) > SIZE_MAX / count) {
        return LT_STATUS_CAPACITY_REACHED;
    }

    ids = (lt_component_id_t*)lt_alloc_bytes(
        &world->allocator,
        sizeof(*ids) * (size_t)count,
        _Alignof(lt_component_id_t));
    if (ids == NULL) {
        return LT_STATUS_ALLOCATION_FAILED;
    }

    src_i = 0u;
    dst_i = 0u;
    for (i = 0u; i < count; ++i) {
        if (src_i >= archetype->component_count
            || (dst_i == 0u && add_id < archetype->component_ids[src_i])) {
            ids[i] = add_id;
            dst_i = 1u;
        } else {
            ids[i] = archetype->component_ids[src_i];
            src_i += 1u;
        }
    }

    if (dst_i == 0u) {
        ids[count - 1u] = add_id;
    }

    *out_ids = ids;
    *out_count = count;
    return LT_STATUS_OK;
}

static lt_status_t lt_world_component_key_with_remove(
    lt_world_t* world,
    const lt_archetype_t* archetype,
    lt_component_id_t remove_id,
    lt_component_id_t** out_ids,
    uint32_t* out_count)
{
    lt_component_id_t* ids;
    uint32_t i;
    uint32_t dst_i;
    uint32_t count;

    if (world == NULL || archetype == NULL || out_ids == NULL || out_count == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    if (archetype->component_count == 0u) {
        return LT_STATUS_NOT_FOUND;
    }

    count = archetype->component_count - 1u;
    ids = NULL;

    if (count > 0u) {
        if (sizeof(*ids) > SIZE_MAX / count) {
            return LT_STATUS_CAPACITY_REACHED;
        }

        ids = (lt_component_id_t*)lt_alloc_bytes(
            &world->allocator,
            sizeof(*ids) * (size_t)count,
            _Alignof(lt_component_id_t));
        if (ids == NULL) {
            return LT_STATUS_ALLOCATION_FAILED;
        }
    }

    dst_i = 0u;
    for (i = 0u; i < archetype->component_count; ++i) {
        if (archetype->component_ids[i] == remove_id) {
            continue;
        }
        if (ids != NULL) {
            ids[dst_i] = archetype->component_ids[i];
        }
        dst_i += 1u;
    }

    *out_ids = ids;
    *out_count = count;
    return LT_STATUS_OK;
}

static lt_status_t lt_query_validate_desc(const lt_world_t* world, const lt_query_desc_t* desc)
{
    uint32_t i;
    uint32_t j;

    if (world == NULL || desc == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    if ((desc->with_count > 0u && desc->with_terms == NULL)
        || (desc->without_count > 0u && desc->without == NULL)) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    for (i = 0u; i < desc->with_count; ++i) {
        lt_component_id_t component_id;

        component_id = desc->with_terms[i].component_id;
        if (component_id == LT_COMPONENT_INVALID || component_id > world->component_count) {
            return LT_STATUS_NOT_FOUND;
        }

        if (desc->with_terms[i].access != LT_ACCESS_READ
            && desc->with_terms[i].access != LT_ACCESS_WRITE) {
            return LT_STATUS_INVALID_ARGUMENT;
        }

        for (j = i + 1u; j < desc->with_count; ++j) {
            if (desc->with_terms[j].component_id == component_id) {
                return LT_STATUS_CONFLICT;
            }
        }
    }

    for (i = 0u; i < desc->without_count; ++i) {
        lt_component_id_t component_id;

        component_id = desc->without[i];
        if (component_id == LT_COMPONENT_INVALID || component_id > world->component_count) {
            return LT_STATUS_NOT_FOUND;
        }

        for (j = i + 1u; j < desc->without_count; ++j) {
            if (desc->without[j] == component_id) {
                return LT_STATUS_CONFLICT;
            }
        }

        for (j = 0u; j < desc->with_count; ++j) {
            if (desc->with_terms[j].component_id == component_id) {
                return LT_STATUS_CONFLICT;
            }
        }
    }

    return LT_STATUS_OK;
}

static lt_status_t lt_query_copy_desc(lt_query_t* query, const lt_query_desc_t* desc)
{
    lt_world_t* world;

    if (query == NULL || query->world == NULL || desc == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    world = query->world;

    if (desc->with_count > 0u) {
        if (sizeof(*query->with_terms) > SIZE_MAX / desc->with_count) {
            return LT_STATUS_CAPACITY_REACHED;
        }

        query->with_terms = (lt_query_term_t*)lt_alloc_bytes(
            &world->allocator,
            sizeof(*query->with_terms) * (size_t)desc->with_count,
            _Alignof(lt_query_term_t));
        if (query->with_terms == NULL) {
            return LT_STATUS_ALLOCATION_FAILED;
        }
        memcpy(
            query->with_terms,
            desc->with_terms,
            sizeof(*query->with_terms) * (size_t)desc->with_count);
        query->with_count = desc->with_count;
    }

    if (desc->without_count > 0u) {
        if (sizeof(*query->without) > SIZE_MAX / desc->without_count) {
            return LT_STATUS_CAPACITY_REACHED;
        }

        query->without = (lt_component_id_t*)lt_alloc_bytes(
            &world->allocator,
            sizeof(*query->without) * (size_t)desc->without_count,
            _Alignof(lt_component_id_t));
        if (query->without == NULL) {
            return LT_STATUS_ALLOCATION_FAILED;
        }
        memcpy(
            query->without,
            desc->without,
            sizeof(*query->without) * (size_t)desc->without_count);
        query->without_count = desc->without_count;
    }

    return LT_STATUS_OK;
}

static int lt_query_matches_archetype(const lt_query_t* query, const lt_archetype_t* archetype)
{
    uint32_t i;

    if (query == NULL || archetype == NULL) {
        return 0;
    }

    for (i = 0u; i < query->with_count; ++i) {
        if (!lt_archetype_find_component_index(
                archetype,
                query->with_terms[i].component_id,
                NULL)) {
            return 0;
        }
    }

    for (i = 0u; i < query->without_count; ++i) {
        if (lt_archetype_find_component_index(archetype, query->without[i], NULL)) {
            return 0;
        }
    }

    return 1;
}

static lt_status_t lt_query_ensure_match_capacity(lt_query_t* query, uint32_t min_capacity)
{
    lt_world_t* world;
    lt_archetype_t** new_matches;
    size_t old_size;
    size_t new_size;

    if (query == NULL || query->world == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    if (query->match_capacity >= min_capacity) {
        return LT_STATUS_OK;
    }

    if (sizeof(*new_matches) > SIZE_MAX / (size_t)min_capacity) {
        return LT_STATUS_CAPACITY_REACHED;
    }

    world = query->world;
    new_size = sizeof(*new_matches) * (size_t)min_capacity;
    new_matches = (lt_archetype_t**)lt_alloc_bytes(
        &world->allocator,
        new_size,
        _Alignof(lt_archetype_t*));
    if (new_matches == NULL) {
        return LT_STATUS_ALLOCATION_FAILED;
    }
    memset(new_matches, 0, new_size);

    if (query->matches != NULL && query->match_count > 0u) {
        old_size = sizeof(*new_matches) * (size_t)query->match_count;
        memcpy(new_matches, query->matches, old_size);
    }

    if (query->matches != NULL) {
        lt_free_bytes(
            &world->allocator,
            query->matches,
            sizeof(*query->matches) * (size_t)query->match_capacity,
            _Alignof(lt_archetype_t*));
    }

    query->matches = new_matches;
    query->match_capacity = min_capacity;
    return LT_STATUS_OK;
}

static lt_status_t lt_query_ensure_scratch_capacity(lt_query_t* query, uint32_t min_capacity)
{
    lt_world_t* world;
    void** new_columns;
    size_t new_size;

    if (query == NULL || query->world == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    if (min_capacity == 0u || query->scratch_capacity >= min_capacity) {
        return LT_STATUS_OK;
    }

    if (sizeof(*new_columns) > SIZE_MAX / (size_t)min_capacity) {
        return LT_STATUS_CAPACITY_REACHED;
    }

    world = query->world;
    new_size = sizeof(*new_columns) * (size_t)min_capacity;
    new_columns = (void**)lt_alloc_bytes(&world->allocator, new_size, _Alignof(void*));
    if (new_columns == NULL) {
        return LT_STATUS_ALLOCATION_FAILED;
    }
    memset(new_columns, 0, new_size);

    if (query->scratch_columns != NULL) {
        if (query->scratch_capacity > 0u) {
            memcpy(
                new_columns,
                query->scratch_columns,
                sizeof(*new_columns) * (size_t)query->scratch_capacity);
        }
        lt_free_bytes(
            &world->allocator,
            query->scratch_columns,
            sizeof(*query->scratch_columns) * (size_t)query->scratch_capacity,
            _Alignof(void*));
    }

    query->scratch_columns = new_columns;
    query->scratch_capacity = min_capacity;
    return LT_STATUS_OK;
}

const char* lt_status_string(lt_status_t status)
{
    switch (status) {
        case LT_STATUS_OK:
            return "LT_STATUS_OK";
        case LT_STATUS_INVALID_ARGUMENT:
            return "LT_STATUS_INVALID_ARGUMENT";
        case LT_STATUS_NOT_FOUND:
            return "LT_STATUS_NOT_FOUND";
        case LT_STATUS_ALREADY_EXISTS:
            return "LT_STATUS_ALREADY_EXISTS";
        case LT_STATUS_CAPACITY_REACHED:
            return "LT_STATUS_CAPACITY_REACHED";
        case LT_STATUS_ALLOCATION_FAILED:
            return "LT_STATUS_ALLOCATION_FAILED";
        case LT_STATUS_STALE_ENTITY:
            return "LT_STATUS_STALE_ENTITY";
        case LT_STATUS_CONFLICT:
            return "LT_STATUS_CONFLICT";
        case LT_STATUS_NOT_IMPLEMENTED:
            return "LT_STATUS_NOT_IMPLEMENTED";
        default:
            return "LT_STATUS_UNKNOWN";
    }
}

lt_status_t lt_world_create(const lt_world_config_t* cfg, lt_world_t** out_world)
{
    lt_world_t* world;
    lt_world_config_t local_cfg;
    lt_allocator_t allocator;
    lt_status_t status;

    if (out_world == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }
    *out_world = NULL;

    memset(&local_cfg, 0, sizeof(local_cfg));
    if (cfg != NULL) {
        local_cfg = *cfg;
    }

    status = lt_prepare_allocator(cfg, &allocator);
    if (status != LT_STATUS_OK) {
        return status;
    }

    world = (lt_world_t*)lt_alloc_bytes(
        &allocator,
        sizeof(*world),
        _Alignof(lt_world_t));
    if (world == NULL) {
        return LT_STATUS_ALLOCATION_FAILED;
    }
    memset(world, 0, sizeof(*world));

    world->allocator = allocator;
    world->target_chunk_bytes =
        local_cfg.target_chunk_bytes == 0u ? LT_DEFAULT_CHUNK_BYTES : local_cfg.target_chunk_bytes;
    world->free_entity_head = UINT32_MAX;

    if (local_cfg.initial_entity_capacity > 0u) {
        status = lt_grow_entities(world, local_cfg.initial_entity_capacity);
        if (status != LT_STATUS_OK) {
            lt_world_destroy(world);
            return status;
        }
    }

    if (local_cfg.initial_component_capacity > 0u) {
        status = lt_grow_components(world, local_cfg.initial_component_capacity);
        if (status != LT_STATUS_OK) {
            lt_world_destroy(world);
            return status;
        }
    }

    status = lt_find_or_create_archetype(world, NULL, 0u, &world->root_archetype);
    if (status != LT_STATUS_OK) {
        lt_world_destroy(world);
        return status;
    }

    *out_world = world;
    return LT_STATUS_OK;
}

void lt_world_destroy(lt_world_t* world)
{
    uint32_t i;

    if (world == NULL) {
        return;
    }

    if (world->archetypes != NULL) {
        for (i = 0u; i < world->archetype_count; ++i) {
            lt_archetype_t* archetype;
            lt_chunk_t* chunk;

            archetype = world->archetypes[i];
            if (archetype == NULL) {
                continue;
            }

            chunk = archetype->chunks;
            while (chunk != NULL) {
                lt_chunk_t* next;
                next = chunk->next;
                lt_chunk_destroy(world, archetype, chunk, 1);
                chunk = next;
            }

            if (archetype->component_ids != NULL) {
                lt_free_bytes(
                    &world->allocator,
                    archetype->component_ids,
                    sizeof(*archetype->component_ids) * (size_t)archetype->component_count,
                    _Alignof(lt_component_id_t));
            }

            lt_free_bytes(
                &world->allocator,
                archetype,
                sizeof(*archetype),
                _Alignof(lt_archetype_t));
        }

        lt_free_bytes(
            &world->allocator,
            world->archetypes,
            sizeof(*world->archetypes) * (size_t)world->archetype_capacity,
            _Alignof(lt_archetype_t*));
        world->archetypes = NULL;
    }

    if (world->components != NULL) {
        for (i = 1u; i <= world->component_count; ++i) {
            lt_component_record_t* record;

            record = &world->components[i];
            if (record->name != NULL) {
                size_t name_size;
                name_size = strlen(record->name) + 1u;
                lt_free_bytes(
                    &world->allocator,
                    record->name,
                    name_size,
                    _Alignof(char));
                record->name = NULL;
            }
        }

        lt_free_bytes(
            &world->allocator,
            world->components,
            sizeof(*world->components) * (size_t)(world->component_capacity + 1u),
            _Alignof(lt_component_record_t));
        world->components = NULL;
    }

    if (world->entities != NULL) {
        lt_free_bytes(
            &world->allocator,
            world->entities,
            sizeof(*world->entities) * (size_t)world->entity_capacity,
            _Alignof(lt_entity_slot_t));
        world->entities = NULL;
    }

    lt_deferred_clear(world);
    if (world->deferred_ops != NULL) {
        lt_free_bytes(
            &world->allocator,
            world->deferred_ops,
            sizeof(*world->deferred_ops) * (size_t)world->deferred_capacity,
            _Alignof(lt_deferred_op_t));
        world->deferred_ops = NULL;
    }

    lt_free_bytes(&world->allocator, world, sizeof(*world), _Alignof(lt_world_t));
}

lt_status_t lt_world_reserve_entities(lt_world_t* world, uint32_t entity_capacity)
{
    if (world == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    return lt_grow_entities(world, entity_capacity);
}

lt_status_t lt_world_reserve_components(lt_world_t* world, uint32_t component_capacity)
{
    if (world == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    return lt_grow_components(world, component_capacity);
}

lt_status_t lt_world_set_trace_hook(lt_world_t* world, lt_trace_hook_fn hook, void* user_data)
{
    if (world == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    world->trace_hook = hook;
    world->trace_user_data = user_data;
    return LT_STATUS_OK;
}

lt_status_t lt_world_begin_defer(lt_world_t* world)
{
    if (world == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    if (world->defer_depth == UINT32_MAX) {
        return LT_STATUS_CAPACITY_REACHED;
    }

    world->defer_depth += 1u;
    lt_trace_emit(
        world,
        LT_TRACE_EVENT_DEFER_BEGIN,
        LT_STATUS_OK,
        LT_ENTITY_NULL,
        LT_COMPONENT_INVALID,
        0u);
    return LT_STATUS_OK;
}

lt_status_t lt_world_end_defer(lt_world_t* world)
{
    if (world == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    if (world->defer_depth == 0u) {
        return LT_STATUS_CONFLICT;
    }

    world->defer_depth -= 1u;
    lt_trace_emit(
        world,
        LT_TRACE_EVENT_DEFER_END,
        LT_STATUS_OK,
        LT_ENTITY_NULL,
        LT_COMPONENT_INVALID,
        0u);
    return LT_STATUS_OK;
}

lt_status_t lt_world_flush(lt_world_t* world)
{
    lt_status_t status;
    uint32_t i;

    if (world == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    if (world->defer_depth != 0u) {
        return LT_STATUS_CONFLICT;
    }

    lt_trace_emit(
        world,
        LT_TRACE_EVENT_FLUSH_BEGIN,
        LT_STATUS_OK,
        LT_ENTITY_NULL,
        LT_COMPONENT_INVALID,
        0u);

    status = LT_STATUS_OK;
    for (i = 0u; i < world->deferred_count; ++i) {
        lt_deferred_op_t* op;

        op = &world->deferred_ops[i];
        switch (op->kind) {
            case LT_DEFERRED_OP_ADD_COMPONENT:
                status = lt_add_component(world, op->entity, op->component_id, op->payload);
                break;
            case LT_DEFERRED_OP_REMOVE_COMPONENT:
                status = lt_remove_component(world, op->entity, op->component_id);
                break;
            case LT_DEFERRED_OP_DESTROY_ENTITY:
                status = lt_entity_destroy(world, op->entity);
                break;
            default:
                status = LT_STATUS_INVALID_ARGUMENT;
                break;
        }

        if (status != LT_STATUS_OK) {
            lt_trace_emit(
                world,
                LT_TRACE_EVENT_FLUSH_APPLY,
                status,
                op->entity,
                op->component_id,
                (uint32_t)op->kind);
            break;
        }

        lt_trace_emit(
            world,
            LT_TRACE_EVENT_FLUSH_APPLY,
            LT_STATUS_OK,
            op->entity,
            op->component_id,
            (uint32_t)op->kind);
    }

    lt_deferred_clear(world);
    lt_trace_emit(
        world,
        LT_TRACE_EVENT_FLUSH_END,
        status,
        LT_ENTITY_NULL,
        LT_COMPONENT_INVALID,
        0u);
    return status;
}

lt_status_t lt_entity_create(lt_world_t* world, lt_entity_t* out_entity)
{
    uint32_t index;
    uint32_t generation;
    lt_entity_slot_t* slot;
    lt_entity_t entity;
    lt_chunk_t* chunk;
    uint32_t row;
    lt_status_t status;
    uint8_t reused_slot;

    if (world == NULL || out_entity == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    reused_slot = 0u;
    if (world->free_entity_head != UINT32_MAX) {
        index = world->free_entity_head;
        slot = &world->entities[index];
        world->free_entity_head = slot->next_free;
        world->free_entity_count -= 1u;
        reused_slot = 1u;
    } else {
        if (world->entity_count == world->entity_capacity) {
            status = lt_grow_entities(world, world->entity_count + 1u);
            if (status != LT_STATUS_OK) {
                return status;
            }
        }

        index = world->entity_count;
        world->entity_count += 1u;
        slot = &world->entities[index];
        memset(slot, 0, sizeof(*slot));
    }

    if (slot->generation == 0u) {
        slot->generation = 1u;
    }

    generation = slot->generation;
    entity = lt_entity_pack(index, generation);

    status = lt_archetype_alloc_row(world, world->root_archetype, &chunk, &row);
    if (status != LT_STATUS_OK) {
        if (reused_slot != 0u) {
            slot->next_free = world->free_entity_head;
            world->free_entity_head = index;
            world->free_entity_count += 1u;
        } else {
            world->entity_count -= 1u;
            memset(slot, 0, sizeof(*slot));
        }
        return status;
    }

    chunk->entities[row] = entity;

    slot->alive = 1u;
    slot->next_free = UINT32_MAX;
    slot->archetype = world->root_archetype;
    slot->chunk = chunk;
    slot->row = row;

    world->live_entity_count += 1u;
    *out_entity = entity;
    lt_trace_emit(
        world,
        LT_TRACE_EVENT_ENTITY_CREATE,
        LT_STATUS_OK,
        entity,
        LT_COMPONENT_INVALID,
        0u);
    return LT_STATUS_OK;
}

lt_status_t lt_entity_destroy(lt_world_t* world, lt_entity_t entity)
{
    lt_entity_slot_t* slot;
    lt_archetype_t* archetype;
    lt_chunk_t* chunk;
    uint32_t row;
    uint32_t i;
    lt_status_t status;

    if (world == NULL || entity == LT_ENTITY_NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    if (world->defer_depth > 0u) {
        return lt_enqueue_destroy_entity(world, entity);
    }

    status = lt_world_get_live_slot(world, entity, &slot);
    if (status != LT_STATUS_OK) {
        lt_trace_emit(
            world,
            LT_TRACE_EVENT_ENTITY_DESTROY,
            status,
            entity,
            LT_COMPONENT_INVALID,
            0u);
        return status;
    }

    archetype = slot->archetype;
    chunk = slot->chunk;
    row = slot->row;

    for (i = 0u; i < archetype->component_count; ++i) {
        lt_component_id_t component_id;
        const lt_component_record_t* component;
        void* ptr;

        component_id = archetype->component_ids[i];
        component = &world->components[component_id];
        if (component->dtor == NULL || component->size == 0u) {
            continue;
        }

        ptr = lt_chunk_component_ptr(world, archetype, chunk, row, i);
        lt_component_destruct_one(component, ptr);
    }

    lt_archetype_swap_remove_row(world, archetype, chunk, row);

    slot->alive = 0u;
    slot->generation += 1u;
    if (slot->generation == 0u) {
        slot->generation = 1u;
    }

    slot->archetype = NULL;
    slot->chunk = NULL;
    slot->row = 0u;

    slot->next_free = world->free_entity_head;
    world->free_entity_head = lt_entity_index(entity);
    world->free_entity_count += 1u;

    if (world->live_entity_count > 0u) {
        world->live_entity_count -= 1u;
    }

    lt_trace_emit(
        world,
        LT_TRACE_EVENT_ENTITY_DESTROY,
        LT_STATUS_OK,
        entity,
        LT_COMPONENT_INVALID,
        0u);
    return LT_STATUS_OK;
}

lt_status_t lt_entity_is_alive(const lt_world_t* world, lt_entity_t entity, uint8_t* out_alive)
{
    uint32_t index;
    uint32_t generation;
    const lt_entity_slot_t* slot;

    if (world == NULL || out_alive == NULL || entity == LT_ENTITY_NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    index = lt_entity_index(entity);
    generation = lt_entity_generation(entity);

    if (index >= world->entity_count) {
        *out_alive = 0u;
        return LT_STATUS_OK;
    }

    slot = &world->entities[index];
    *out_alive = (uint8_t)(slot->alive != 0u && slot->generation == generation);
    return LT_STATUS_OK;
}

lt_status_t lt_add_component(
    lt_world_t* world,
    lt_entity_t entity,
    lt_component_id_t component_id,
    const void* initial_value)
{
    lt_entity_slot_t* slot;
    lt_archetype_t* src_archetype;
    lt_chunk_t* src_chunk;
    uint32_t src_row;
    lt_component_id_t* dst_ids;
    uint32_t dst_count;
    lt_archetype_t* dst_archetype;
    lt_chunk_t* dst_chunk;
    uint32_t dst_row;
    uint32_t i;
    lt_status_t status;

    if (world == NULL || entity == LT_ENTITY_NULL || component_id == LT_COMPONENT_INVALID) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    if (component_id > world->component_count) {
        return LT_STATUS_NOT_FOUND;
    }

    if (world->defer_depth > 0u) {
        return lt_enqueue_add_component(world, entity, component_id, initial_value);
    }

    status = lt_world_get_live_slot(world, entity, &slot);
    if (status != LT_STATUS_OK) {
        lt_trace_emit(
            world,
            LT_TRACE_EVENT_COMPONENT_ADD,
            status,
            entity,
            component_id,
            0u);
        return status;
    }

    src_archetype = slot->archetype;
    src_chunk = slot->chunk;
    src_row = slot->row;

    if (lt_archetype_find_component_index(src_archetype, component_id, NULL)) {
        lt_trace_emit(
            world,
            LT_TRACE_EVENT_COMPONENT_ADD,
            LT_STATUS_ALREADY_EXISTS,
            entity,
            component_id,
            0u);
        return LT_STATUS_ALREADY_EXISTS;
    }

    dst_ids = NULL;
    dst_count = 0u;
    status = lt_world_component_key_with_add(world, src_archetype, component_id, &dst_ids, &dst_count);
    if (status != LT_STATUS_OK) {
        return status;
    }

    status = lt_find_or_create_archetype(world, dst_ids, dst_count, &dst_archetype);
    if (dst_ids != NULL) {
        lt_free_bytes(
            &world->allocator,
            dst_ids,
            sizeof(*dst_ids) * (size_t)dst_count,
            _Alignof(lt_component_id_t));
    }
    if (status != LT_STATUS_OK) {
        return status;
    }

    status = lt_archetype_alloc_row(world, dst_archetype, &dst_chunk, &dst_row);
    if (status != LT_STATUS_OK) {
        return status;
    }

    dst_chunk->entities[dst_row] = entity;

    for (i = 0u; i < dst_archetype->component_count; ++i) {
        lt_component_id_t dst_component_id;
        const lt_component_record_t* component;
        void* dst_ptr;

        dst_component_id = dst_archetype->component_ids[i];
        component = &world->components[dst_component_id];
        dst_ptr = lt_chunk_component_ptr(world, dst_archetype, dst_chunk, dst_row, i);

        if (dst_component_id == component_id) {
            lt_component_init_added(component, dst_ptr, initial_value);
        } else {
            uint32_t src_i;
            void* src_ptr;

            (void)lt_archetype_find_component_index(src_archetype, dst_component_id, &src_i);
            src_ptr = lt_chunk_component_ptr(world, src_archetype, src_chunk, src_row, src_i);
            lt_component_transfer(component, dst_ptr, src_ptr);
        }
    }

    slot->archetype = dst_archetype;
    slot->chunk = dst_chunk;
    slot->row = dst_row;
    world->structural_move_count += 1u;

    lt_archetype_swap_remove_row(world, src_archetype, src_chunk, src_row);
    lt_trace_emit(
        world,
        LT_TRACE_EVENT_COMPONENT_ADD,
        LT_STATUS_OK,
        entity,
        component_id,
        0u);
    return LT_STATUS_OK;
}

lt_status_t lt_remove_component(
    lt_world_t* world,
    lt_entity_t entity,
    lt_component_id_t component_id)
{
    lt_entity_slot_t* slot;
    lt_archetype_t* src_archetype;
    lt_chunk_t* src_chunk;
    uint32_t src_row;
    lt_component_id_t* dst_ids;
    uint32_t dst_count;
    lt_archetype_t* dst_archetype;
    lt_chunk_t* dst_chunk;
    uint32_t dst_row;
    uint32_t removed_index;
    uint32_t i;
    lt_status_t status;

    if (world == NULL || entity == LT_ENTITY_NULL || component_id == LT_COMPONENT_INVALID) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    if (component_id > world->component_count) {
        return LT_STATUS_NOT_FOUND;
    }

    if (world->defer_depth > 0u) {
        return lt_enqueue_remove_component(world, entity, component_id);
    }

    status = lt_world_get_live_slot(world, entity, &slot);
    if (status != LT_STATUS_OK) {
        lt_trace_emit(
            world,
            LT_TRACE_EVENT_COMPONENT_REMOVE,
            status,
            entity,
            component_id,
            0u);
        return status;
    }

    src_archetype = slot->archetype;
    src_chunk = slot->chunk;
    src_row = slot->row;

    if (!lt_archetype_find_component_index(src_archetype, component_id, &removed_index)) {
        lt_trace_emit(
            world,
            LT_TRACE_EVENT_COMPONENT_REMOVE,
            LT_STATUS_NOT_FOUND,
            entity,
            component_id,
            0u);
        return LT_STATUS_NOT_FOUND;
    }

    dst_ids = NULL;
    dst_count = 0u;
    status = lt_world_component_key_with_remove(world, src_archetype, component_id, &dst_ids, &dst_count);
    if (status != LT_STATUS_OK) {
        return status;
    }

    status = lt_find_or_create_archetype(world, dst_ids, dst_count, &dst_archetype);
    if (dst_ids != NULL) {
        lt_free_bytes(
            &world->allocator,
            dst_ids,
            sizeof(*dst_ids) * (size_t)dst_count,
            _Alignof(lt_component_id_t));
    }
    if (status != LT_STATUS_OK) {
        return status;
    }

    status = lt_archetype_alloc_row(world, dst_archetype, &dst_chunk, &dst_row);
    if (status != LT_STATUS_OK) {
        return status;
    }

    dst_chunk->entities[dst_row] = entity;

    for (i = 0u; i < dst_archetype->component_count; ++i) {
        lt_component_id_t dst_component_id;
        const lt_component_record_t* component;
        uint32_t src_i;
        void* src_ptr;
        void* dst_ptr;

        dst_component_id = dst_archetype->component_ids[i];
        component = &world->components[dst_component_id];
        (void)lt_archetype_find_component_index(src_archetype, dst_component_id, &src_i);

        src_ptr = lt_chunk_component_ptr(world, src_archetype, src_chunk, src_row, src_i);
        dst_ptr = lt_chunk_component_ptr(world, dst_archetype, dst_chunk, dst_row, i);
        lt_component_transfer(component, dst_ptr, src_ptr);
    }

    {
        const lt_component_record_t* removed_component;
        void* removed_ptr;

        removed_component = &world->components[component_id];
        removed_ptr = lt_chunk_component_ptr(world, src_archetype, src_chunk, src_row, removed_index);
        lt_component_destruct_one(removed_component, removed_ptr);
    }

    slot->archetype = dst_archetype;
    slot->chunk = dst_chunk;
    slot->row = dst_row;
    world->structural_move_count += 1u;

    lt_archetype_swap_remove_row(world, src_archetype, src_chunk, src_row);
    lt_trace_emit(
        world,
        LT_TRACE_EVENT_COMPONENT_REMOVE,
        LT_STATUS_OK,
        entity,
        component_id,
        0u);
    return LT_STATUS_OK;
}

lt_status_t lt_has_component(
    const lt_world_t* world,
    lt_entity_t entity,
    lt_component_id_t component_id,
    uint8_t* out_has)
{
    lt_entity_slot_t* slot;
    lt_status_t status;

    if (world == NULL || out_has == NULL || entity == LT_ENTITY_NULL || component_id == LT_COMPONENT_INVALID) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    if (component_id > world->component_count) {
        *out_has = 0u;
        return LT_STATUS_OK;
    }

    status = lt_world_get_live_slot(world, entity, &slot);
    if (status != LT_STATUS_OK) {
        return status;
    }

    *out_has = (uint8_t)lt_archetype_find_component_index(slot->archetype, component_id, NULL);
    return LT_STATUS_OK;
}

lt_status_t lt_get_component(
    lt_world_t* world,
    lt_entity_t entity,
    lt_component_id_t component_id,
    void** out_ptr)
{
    lt_entity_slot_t* slot;
    uint32_t component_index;
    lt_status_t status;

    if (world == NULL
        || out_ptr == NULL
        || entity == LT_ENTITY_NULL
        || component_id == LT_COMPONENT_INVALID) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    *out_ptr = NULL;

    if (component_id > world->component_count) {
        return LT_STATUS_NOT_FOUND;
    }

    status = lt_world_get_live_slot(world, entity, &slot);
    if (status != LT_STATUS_OK) {
        return status;
    }

    if (!lt_archetype_find_component_index(slot->archetype, component_id, &component_index)) {
        return LT_STATUS_NOT_FOUND;
    }

    *out_ptr = lt_chunk_component_ptr(world, slot->archetype, slot->chunk, slot->row, component_index);
    return LT_STATUS_OK;
}

lt_status_t lt_register_component(
    lt_world_t* world,
    const lt_component_desc_t* desc,
    lt_component_id_t* out_id)
{
    uint32_t i;
    lt_component_id_t id;
    lt_status_t status;
    lt_component_record_t* record;

    if (world == NULL || desc == NULL || out_id == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    status = lt_component_desc_validate(desc);
    if (status != LT_STATUS_OK) {
        return status;
    }

    for (i = 1u; i <= world->component_count; ++i) {
        if (lt_component_names_equal(world->components[i].name, desc->name)) {
            return LT_STATUS_ALREADY_EXISTS;
        }
    }

    if (world->component_count == UINT32_MAX - 1u) {
        return LT_STATUS_CAPACITY_REACHED;
    }

    id = world->component_count + 1u;
    status = lt_grow_components(world, id);
    if (status != LT_STATUS_OK) {
        return status;
    }

    record = &world->components[id];
    memset(record, 0, sizeof(*record));

    status = lt_component_name_copy(world, desc->name, &record->name);
    if (status != LT_STATUS_OK) {
        return status;
    }

    record->size = desc->size;
    record->align = desc->align == 0u ? 1u : desc->align;
    record->flags = desc->flags;
    record->ctor = desc->ctor;
    record->dtor = desc->dtor;
    record->move = desc->move;
    record->user = desc->user;

    world->component_count = id;
    *out_id = id;
    return LT_STATUS_OK;
}

lt_status_t lt_query_create(lt_world_t* world, const lt_query_desc_t* desc, lt_query_t** out_query)
{
    lt_query_t* query;
    lt_status_t status;

    if (world == NULL || desc == NULL || out_query == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }
    *out_query = NULL;

    status = lt_query_validate_desc(world, desc);
    if (status != LT_STATUS_OK) {
        return status;
    }

    query = (lt_query_t*)lt_alloc_bytes(&world->allocator, sizeof(*query), _Alignof(lt_query_t));
    if (query == NULL) {
        return LT_STATUS_ALLOCATION_FAILED;
    }
    memset(query, 0, sizeof(*query));
    query->world = world;

    status = lt_query_copy_desc(query, desc);
    if (status != LT_STATUS_OK) {
        lt_query_destroy(query);
        return status;
    }

    status = lt_query_ensure_scratch_capacity(query, query->with_count);
    if (status != LT_STATUS_OK) {
        lt_query_destroy(query);
        return status;
    }

    status = lt_query_refresh(query);
    if (status != LT_STATUS_OK) {
        lt_query_destroy(query);
        return status;
    }

    *out_query = query;
    return LT_STATUS_OK;
}

void lt_query_destroy(lt_query_t* query)
{
    lt_world_t* world;

    if (query == NULL) {
        return;
    }

    world = query->world;
    if (world != NULL) {
        if (query->with_terms != NULL) {
            lt_free_bytes(
                &world->allocator,
                query->with_terms,
                sizeof(*query->with_terms) * (size_t)query->with_count,
                _Alignof(lt_query_term_t));
        }
        if (query->without != NULL) {
            lt_free_bytes(
                &world->allocator,
                query->without,
                sizeof(*query->without) * (size_t)query->without_count,
                _Alignof(lt_component_id_t));
        }
        if (query->matches != NULL) {
            lt_free_bytes(
                &world->allocator,
                query->matches,
                sizeof(*query->matches) * (size_t)query->match_capacity,
                _Alignof(lt_archetype_t*));
        }
        if (query->scratch_columns != NULL) {
            lt_free_bytes(
                &world->allocator,
                query->scratch_columns,
                sizeof(*query->scratch_columns) * (size_t)query->scratch_capacity,
                _Alignof(void*));
        }
        lt_free_bytes(&world->allocator, query, sizeof(*query), _Alignof(lt_query_t));
    }
}

lt_status_t lt_query_refresh(lt_query_t* query)
{
    lt_world_t* world;
    uint32_t i;
    lt_status_t status;

    if (query == NULL || query->world == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    world = query->world;
    status = lt_query_ensure_match_capacity(query, world->archetype_count);
    if (status != LT_STATUS_OK) {
        return status;
    }

    query->match_count = 0u;
    for (i = 0u; i < world->archetype_count; ++i) {
        lt_archetype_t* archetype;

        archetype = world->archetypes[i];
        if (archetype == NULL) {
            continue;
        }

        if (lt_query_matches_archetype(query, archetype)) {
            query->matches[query->match_count] = archetype;
            query->match_count += 1u;
        }
    }

    return LT_STATUS_OK;
}

lt_status_t lt_query_iter_begin(lt_query_t* query, lt_query_iter_t* out_iter)
{
    lt_world_t* world;
    lt_status_t status;

    if (query == NULL || out_iter == NULL || query->world == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }
    world = query->world;

    status = lt_query_refresh(query);
    if (status != LT_STATUS_OK) {
        lt_trace_emit(
            world,
            LT_TRACE_EVENT_QUERY_ITER_BEGIN,
            status,
            LT_ENTITY_NULL,
            LT_COMPONENT_INVALID,
            query->match_count);
        return status;
    }

    status = lt_query_ensure_scratch_capacity(query, query->with_count);
    if (status != LT_STATUS_OK) {
        lt_trace_emit(
            world,
            LT_TRACE_EVENT_QUERY_ITER_BEGIN,
            status,
            LT_ENTITY_NULL,
            LT_COMPONENT_INVALID,
            query->match_count);
        return status;
    }

    memset(out_iter, 0, sizeof(*out_iter));
    out_iter->query = query;
    out_iter->archetype_index = 0u;
    out_iter->chunk_cursor = NULL;
    out_iter->columns = query->scratch_columns;
    out_iter->column_capacity = query->scratch_capacity;
    out_iter->finished = 0u;
    lt_trace_emit(
        world,
        LT_TRACE_EVENT_QUERY_ITER_BEGIN,
        LT_STATUS_OK,
        LT_ENTITY_NULL,
        LT_COMPONENT_INVALID,
        query->match_count);
    return LT_STATUS_OK;
}

lt_status_t lt_query_iter_next(
    lt_query_iter_t* iter,
    lt_chunk_view_t* out_view,
    uint8_t* out_has_value)
{
    lt_query_t* query;
    lt_world_t* world;
    lt_status_t status;

    if (iter == NULL || out_view == NULL || out_has_value == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    query = iter->query;
    if (query == NULL || query->world == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }
    world = query->world;

    out_view->count = 0u;
    out_view->entities = NULL;
    out_view->columns = NULL;
    out_view->column_count = 0u;
    *out_has_value = 0u;

    status = lt_query_ensure_scratch_capacity(query, query->with_count);
    if (status != LT_STATUS_OK) {
        lt_trace_emit(
            world,
            LT_TRACE_EVENT_QUERY_ITER_END,
            status,
            LT_ENTITY_NULL,
            LT_COMPONENT_INVALID,
            query->match_count);
        return status;
    }

    if (iter->finished != 0u) {
        return LT_STATUS_OK;
    }

    while (iter->archetype_index < query->match_count) {
        lt_archetype_t* archetype;
        lt_chunk_t* chunk;
        uint32_t i;

        archetype = query->matches[iter->archetype_index];
        chunk = (lt_chunk_t*)iter->chunk_cursor;
        if (chunk == NULL) {
            chunk = archetype->chunks;
        }

        while (chunk != NULL && chunk->count == 0u) {
            chunk = chunk->next;
        }

        if (chunk == NULL) {
            iter->archetype_index += 1u;
            iter->chunk_cursor = NULL;
            continue;
        }

        for (i = 0u; i < query->with_count; ++i) {
            uint32_t component_index;
            int found;

            found = lt_archetype_find_component_index(
                archetype,
                query->with_terms[i].component_id,
                &component_index);
            if (!found) {
                iter->finished = 1u;
                lt_trace_emit(
                    world,
                    LT_TRACE_EVENT_QUERY_ITER_END,
                    LT_STATUS_CONFLICT,
                    LT_ENTITY_NULL,
                    LT_COMPONENT_INVALID,
                    query->match_count);
                return LT_STATUS_CONFLICT;
            }

            query->scratch_columns[i] = lt_chunk_component_ptr(
                world,
                archetype,
                chunk,
                0u,
                component_index);
        }

        out_view->count = chunk->count;
        out_view->entities = chunk->entities;
        out_view->columns = query->scratch_columns;
        out_view->column_count = query->with_count;
        *out_has_value = 1u;

        iter->columns = query->scratch_columns;
        iter->column_capacity = query->scratch_capacity;
        iter->finished = 0u;
        iter->chunk_cursor = chunk->next;
        if (iter->chunk_cursor == NULL) {
            iter->archetype_index += 1u;
        }
        lt_trace_emit(
            world,
            LT_TRACE_EVENT_QUERY_ITER_CHUNK,
            LT_STATUS_OK,
            LT_ENTITY_NULL,
            LT_COMPONENT_INVALID,
            out_view->count);
        return LT_STATUS_OK;
    }

    iter->finished = 1u;
    lt_trace_emit(
        world,
        LT_TRACE_EVENT_QUERY_ITER_END,
        LT_STATUS_OK,
        LT_ENTITY_NULL,
        LT_COMPONENT_INVALID,
        query->match_count);
    return LT_STATUS_OK;
}

lt_status_t lt_world_get_stats(const lt_world_t* world, lt_world_stats_t* out_stats)
{
    if (world == NULL || out_stats == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    out_stats->live_entities = world->live_entity_count;
    out_stats->entity_capacity = world->entity_capacity;
    out_stats->allocated_entity_slots = world->entity_count;
    out_stats->free_entity_slots = world->free_entity_count;
    out_stats->registered_components = world->component_count;
    out_stats->archetype_count = world->archetype_count;
    out_stats->chunk_count = world->total_chunk_count;
    out_stats->pending_commands = world->deferred_count;
    out_stats->defer_depth = world->defer_depth;
    out_stats->structural_moves = world->structural_move_count;
    return LT_STATUS_OK;
}
