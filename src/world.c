#include "lattice/world.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifndef __STDC_VERSION__
#error "C11 or newer is required"
#endif

typedef struct lt_entity_slot_s {
    uint32_t generation;
    uint32_t next_free;
    uint8_t alive;
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

struct lt_world_s {
    lt_allocator_t allocator;

    lt_entity_slot_t* entities;
    uint32_t entity_capacity;
    uint32_t entity_count;
    uint32_t live_entity_count;
    uint32_t free_entity_count;
    uint32_t free_entity_head;

    lt_component_record_t* components;
    uint32_t component_capacity;
    uint32_t component_count;
};

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

    *out_world = world;
    return LT_STATUS_OK;
}

void lt_world_destroy(lt_world_t* world)
{
    uint32_t i;

    if (world == NULL) {
        return;
    }

    if (world->components != NULL) {
        for (i = 1u; i <= world->component_count; ++i) {
            lt_component_record_t* record = &world->components[i];
            if (record->name != NULL) {
                size_t name_size = strlen(record->name) + 1u;
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

lt_status_t lt_entity_create(lt_world_t* world, lt_entity_t* out_entity)
{
    uint32_t index;
    uint32_t generation;
    lt_entity_slot_t* slot;
    lt_status_t status;

    if (world == NULL || out_entity == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    if (world->free_entity_head != UINT32_MAX) {
        index = world->free_entity_head;
        slot = &world->entities[index];
        world->free_entity_head = slot->next_free;
        world->free_entity_count -= 1u;

        slot->alive = 1u;
        slot->next_free = UINT32_MAX;
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
        slot->alive = 1u;
        slot->next_free = UINT32_MAX;
        if (slot->generation == 0u) {
            slot->generation = 1u;
        }
    }

    generation = slot->generation;
    world->live_entity_count += 1u;
    *out_entity = lt_entity_pack(index, generation);
    return LT_STATUS_OK;
}

lt_status_t lt_entity_destroy(lt_world_t* world, lt_entity_t entity)
{
    uint32_t index;
    uint32_t generation;
    lt_entity_slot_t* slot;

    if (world == NULL || entity == LT_ENTITY_NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    index = lt_entity_index(entity);
    generation = lt_entity_generation(entity);

    if (index >= world->entity_count) {
        return LT_STATUS_STALE_ENTITY;
    }

    slot = &world->entities[index];
    if (slot->alive == 0u || slot->generation != generation) {
        return LT_STATUS_STALE_ENTITY;
    }

    slot->alive = 0u;
    slot->generation += 1u;
    if (slot->generation == 0u) {
        slot->generation = 1u;
    }

    slot->next_free = world->free_entity_head;
    world->free_entity_head = index;
    world->free_entity_count += 1u;

    if (world->live_entity_count > 0u) {
        world->live_entity_count -= 1u;
    }

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
    record->align = desc->align;
    record->flags = desc->flags;
    record->ctor = desc->ctor;
    record->dtor = desc->dtor;
    record->move = desc->move;
    record->user = desc->user;

    world->component_count = id;
    *out_id = id;
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
    return LT_STATUS_OK;
}
