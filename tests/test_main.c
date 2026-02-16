#include "lattice/lattice.h"

#include <stdio.h>
#include <string.h>

#define ASSERT_TRUE(condition)                                                      \
    do {                                                                            \
        if (!(condition)) {                                                         \
            fprintf(stderr, "Assertion failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #condition);                                                    \
            return 1;                                                               \
        }                                                                           \
    } while (0)

#define ASSERT_STATUS(actual, expected)                                             \
    do {                                                                            \
        lt_status_t status_result = (actual);                                       \
        if (status_result != (expected)) {                                          \
            fprintf(stderr, "Unexpected status at %s:%d: got %s expected %s\n",    \
                    __FILE__, __LINE__, lt_status_string(status_result),            \
                    lt_status_string((expected)));                                  \
            return 1;                                                               \
        }                                                                           \
    } while (0)

#define RUN_TEST(fn)                                                                \
    do {                                                                            \
        int fn_result = (fn)();                                                     \
        if (fn_result != 0) {                                                       \
            fprintf(stderr, "Test failed: %s\n", #fn);                            \
            return fn_result;                                                       \
        }                                                                           \
    } while (0)

typedef struct test_vec3_s {
    float x;
    float y;
    float z;
} test_vec3_t;

typedef struct test_trace_capture_s {
    uint32_t total;
    uint32_t defer_begin_count;
    uint32_t defer_end_count;
    uint32_t defer_enqueue_count;
    uint32_t flush_begin_count;
    uint32_t flush_apply_count;
    uint32_t flush_end_count;
    uint32_t entity_create_count;
    uint32_t entity_destroy_count;
    uint32_t component_add_count;
    uint32_t component_remove_count;
    uint32_t query_begin_count;
    uint32_t query_chunk_count;
    uint32_t query_end_count;
    lt_status_t last_status;
    lt_trace_event_kind_t last_kind;
} test_trace_capture_t;

typedef struct test_entity_state_s {
    lt_entity_t entity;
    uint8_t alive;
    uint8_t has_position;
    uint8_t has_velocity;
} test_entity_state_t;

typedef struct test_determinism_snapshot_s {
    uint64_t checksum;
    lt_world_stats_t stats;
    uint32_t tracked_alive_count;
} test_determinism_snapshot_t;

static void* test_alloc_only(void* user, size_t size, size_t align)
{
    (void)user;
    (void)size;
    (void)align;
    return NULL;
}

static void test_counting_dtor(void* dst, uint32_t count, void* user)
{
    int* total;

    (void)dst;

    total = (int*)user;
    *total += (int)count;
}

static void test_trace_hook(const lt_trace_event_t* event, void* user_data)
{
    test_trace_capture_t* capture;

    if (event == NULL || user_data == NULL) {
        return;
    }

    capture = (test_trace_capture_t*)user_data;
    capture->total += 1u;
    capture->last_status = event->status;
    capture->last_kind = event->kind;

    switch (event->kind) {
        case LT_TRACE_EVENT_DEFER_BEGIN:
            capture->defer_begin_count += 1u;
            break;
        case LT_TRACE_EVENT_DEFER_END:
            capture->defer_end_count += 1u;
            break;
        case LT_TRACE_EVENT_DEFER_ENQUEUE:
            capture->defer_enqueue_count += 1u;
            break;
        case LT_TRACE_EVENT_FLUSH_BEGIN:
            capture->flush_begin_count += 1u;
            break;
        case LT_TRACE_EVENT_FLUSH_APPLY:
            capture->flush_apply_count += 1u;
            break;
        case LT_TRACE_EVENT_FLUSH_END:
            capture->flush_end_count += 1u;
            break;
        case LT_TRACE_EVENT_ENTITY_CREATE:
            capture->entity_create_count += 1u;
            break;
        case LT_TRACE_EVENT_ENTITY_DESTROY:
            capture->entity_destroy_count += 1u;
            break;
        case LT_TRACE_EVENT_COMPONENT_ADD:
            capture->component_add_count += 1u;
            break;
        case LT_TRACE_EVENT_COMPONENT_REMOVE:
            capture->component_remove_count += 1u;
            break;
        case LT_TRACE_EVENT_QUERY_ITER_BEGIN:
            capture->query_begin_count += 1u;
            break;
        case LT_TRACE_EVENT_QUERY_ITER_CHUNK:
            capture->query_chunk_count += 1u;
            break;
        case LT_TRACE_EVENT_QUERY_ITER_END:
            capture->query_end_count += 1u;
            break;
        default:
            break;
    }
}

static uint32_t test_rand_u32(uint32_t* state)
{
    *state = (*state * 1664525u) + 1013904223u;
    return *state;
}

static float test_rand_range(uint32_t* state, float min_value, float max_value)
{
    float t;
    uint32_t r;

    r = test_rand_u32(state);
    t = (float)(r >> 8) / (float)0x00FFFFFFu;
    return min_value + ((max_value - min_value) * t);
}

static uint64_t test_checksum_mix(uint64_t hash, uint64_t value)
{
    return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u));
}

static uint32_t test_float_bits(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static int test_pick_alive_index(
    const test_entity_state_t* states,
    uint32_t state_count,
    uint32_t start,
    uint32_t* out_index)
{
    uint32_t i;

    if (states == NULL || out_index == NULL || state_count == 0u) {
        return 0;
    }

    for (i = 0u; i < state_count; ++i) {
        uint32_t idx;

        idx = (start + i) % state_count;
        if (states[idx].alive != 0u) {
            *out_index = idx;
            return 1;
        }
    }

    return 0;
}

static int register_vec3_components(
    lt_world_t* world,
    lt_component_id_t* out_position,
    lt_component_id_t* out_velocity)
{
    lt_component_desc_t desc;

    memset(&desc, 0, sizeof(desc));
    desc.name = "Position";
    desc.size = (uint32_t)sizeof(test_vec3_t);
    desc.align = (uint32_t)_Alignof(test_vec3_t);
    ASSERT_STATUS(lt_register_component(world, &desc, out_position), LT_STATUS_OK);

    desc.name = "Velocity";
    ASSERT_STATUS(lt_register_component(world, &desc, out_velocity), LT_STATUS_OK);
    return 0;
}

static int run_seeded_determinism_sequence(
    uint32_t seed,
    test_determinism_snapshot_t* out_snapshot)
{
    enum {
        INITIAL_ENTITY_COUNT = 24u,
        FRAME_COUNT = 32u,
        MAX_TRACKED_ENTITIES = 256u
    };
    lt_world_t* world;
    lt_component_id_t position_id;
    lt_component_id_t velocity_id;
    lt_query_term_t terms[2];
    lt_query_desc_t query_desc;
    lt_query_t* query;
    test_entity_state_t states[MAX_TRACKED_ENTITIES];
    lt_world_stats_t stats;
    uint32_t rng;
    uint64_t checksum;
    uint32_t state_count;
    uint32_t frame;
    uint32_t i;

    ASSERT_TRUE(out_snapshot != NULL);
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    memset(states, 0, sizeof(states));
    rng = seed;
    checksum = 0xcbf29ce484222325ull;
    state_count = 0u;

    ASSERT_STATUS(lt_world_create(NULL, &world), LT_STATUS_OK);
    ASSERT_TRUE(register_vec3_components(world, &position_id, &velocity_id) == 0);

    memset(terms, 0, sizeof(terms));
    terms[0].component_id = position_id;
    terms[0].access = LT_ACCESS_WRITE;
    terms[1].component_id = velocity_id;
    terms[1].access = LT_ACCESS_READ;

    memset(&query_desc, 0, sizeof(query_desc));
    query_desc.with_terms = terms;
    query_desc.with_count = 2u;

    ASSERT_STATUS(lt_query_create(world, &query_desc, &query), LT_STATUS_OK);

    for (i = 0u; i < INITIAL_ENTITY_COUNT; ++i) {
        lt_entity_t entity;
        test_vec3_t position;
        test_vec3_t velocity;

        ASSERT_TRUE(state_count < MAX_TRACKED_ENTITIES);
        ASSERT_STATUS(lt_entity_create(world, &entity), LT_STATUS_OK);

        states[state_count].entity = entity;
        states[state_count].alive = 1u;
        states[state_count].has_position = 0u;
        states[state_count].has_velocity = 0u;

        position.x = test_rand_range(&rng, -100.0f, 100.0f);
        position.y = test_rand_range(&rng, -100.0f, 100.0f);
        position.z = test_rand_range(&rng, -100.0f, 100.0f);
        ASSERT_STATUS(lt_add_component(world, entity, position_id, &position), LT_STATUS_OK);
        states[state_count].has_position = 1u;

        if ((test_rand_u32(&rng) & 1u) != 0u) {
            velocity.x = test_rand_range(&rng, -5.0f, 5.0f);
            velocity.y = test_rand_range(&rng, -5.0f, 5.0f);
            velocity.z = test_rand_range(&rng, -5.0f, 5.0f);
            ASSERT_STATUS(lt_add_component(world, entity, velocity_id, &velocity), LT_STATUS_OK);
            states[state_count].has_velocity = 1u;
        }

        state_count += 1u;
    }

    for (frame = 0u; frame < FRAME_COUNT; ++frame) {
        lt_query_iter_t iter;
        lt_chunk_view_t view;
        uint8_t has_value;
        uint32_t spawn_count;
        uint32_t op_count;

        ASSERT_STATUS(lt_query_iter_begin(query, &iter), LT_STATUS_OK);
        while (1) {
            ASSERT_STATUS(lt_query_iter_next(&iter, &view, &has_value), LT_STATUS_OK);
            if (has_value == 0u) {
                break;
            }

            for (i = 0u; i < view.count; ++i) {
                test_vec3_t* position_col;
                test_vec3_t* velocity_col;

                position_col = (test_vec3_t*)view.columns[0];
                velocity_col = (test_vec3_t*)view.columns[1];
                position_col[i].x += velocity_col[i].x * (1.0f / 60.0f);
                position_col[i].y += velocity_col[i].y * (1.0f / 90.0f);
                position_col[i].z -= velocity_col[i].z * (1.0f / 120.0f);

                checksum = test_checksum_mix(checksum, (uint64_t)(uint32_t)view.entities[i]);
                checksum = test_checksum_mix(checksum, (uint64_t)test_float_bits(position_col[i].x));
                checksum = test_checksum_mix(checksum, (uint64_t)test_float_bits(position_col[i].y));
                checksum = test_checksum_mix(checksum, (uint64_t)test_float_bits(position_col[i].z));
            }
        }

        spawn_count = test_rand_u32(&rng) % 3u;
        for (i = 0u; i < spawn_count; ++i) {
            lt_entity_t entity;
            test_vec3_t position;
            test_vec3_t velocity;

            if (state_count >= MAX_TRACKED_ENTITIES) {
                break;
            }

            ASSERT_STATUS(lt_entity_create(world, &entity), LT_STATUS_OK);
            states[state_count].entity = entity;
            states[state_count].alive = 1u;
            states[state_count].has_position = 0u;
            states[state_count].has_velocity = 0u;

            position.x = test_rand_range(&rng, -100.0f, 100.0f);
            position.y = test_rand_range(&rng, -100.0f, 100.0f);
            position.z = test_rand_range(&rng, -100.0f, 100.0f);
            ASSERT_STATUS(lt_add_component(world, entity, position_id, &position), LT_STATUS_OK);
            states[state_count].has_position = 1u;

            if ((test_rand_u32(&rng) % 3u) != 0u) {
                velocity.x = test_rand_range(&rng, -5.0f, 5.0f);
                velocity.y = test_rand_range(&rng, -5.0f, 5.0f);
                velocity.z = test_rand_range(&rng, -5.0f, 5.0f);
                ASSERT_STATUS(lt_add_component(world, entity, velocity_id, &velocity), LT_STATUS_OK);
                states[state_count].has_velocity = 1u;
            }

            state_count += 1u;
        }

        ASSERT_STATUS(lt_world_begin_defer(world), LT_STATUS_OK);
        op_count = 1u + (test_rand_u32(&rng) % 5u);
        for (i = 0u; i < op_count; ++i) {
            uint32_t idx;
            uint32_t op_kind;

            if (!test_pick_alive_index(states, state_count, test_rand_u32(&rng), &idx)) {
                break;
            }

            op_kind = test_rand_u32(&rng) % 4u;
            if (op_kind == 0u) {
                if (states[idx].has_velocity != 0u) {
                    ASSERT_STATUS(
                        lt_remove_component(world, states[idx].entity, velocity_id),
                        LT_STATUS_OK);
                    states[idx].has_velocity = 0u;
                } else {
                    test_vec3_t velocity;

                    velocity.x = test_rand_range(&rng, -5.0f, 5.0f);
                    velocity.y = test_rand_range(&rng, -5.0f, 5.0f);
                    velocity.z = test_rand_range(&rng, -5.0f, 5.0f);
                    ASSERT_STATUS(
                        lt_add_component(world, states[idx].entity, velocity_id, &velocity),
                        LT_STATUS_OK);
                    states[idx].has_velocity = 1u;
                }
            } else if (op_kind == 1u) {
                if (states[idx].has_position != 0u) {
                    ASSERT_STATUS(
                        lt_remove_component(world, states[idx].entity, position_id),
                        LT_STATUS_OK);
                    states[idx].has_position = 0u;
                } else {
                    test_vec3_t position;

                    position.x = test_rand_range(&rng, -100.0f, 100.0f);
                    position.y = test_rand_range(&rng, -100.0f, 100.0f);
                    position.z = test_rand_range(&rng, -100.0f, 100.0f);
                    ASSERT_STATUS(
                        lt_add_component(world, states[idx].entity, position_id, &position),
                        LT_STATUS_OK);
                    states[idx].has_position = 1u;
                }
            } else if (op_kind == 2u) {
                if (states[idx].has_position != 0u && states[idx].has_velocity != 0u) {
                    ASSERT_STATUS(
                        lt_remove_component(world, states[idx].entity, velocity_id),
                        LT_STATUS_OK);
                    states[idx].has_velocity = 0u;
                } else if (states[idx].has_position != 0u) {
                    test_vec3_t velocity;

                    velocity.x = test_rand_range(&rng, -5.0f, 5.0f);
                    velocity.y = test_rand_range(&rng, -5.0f, 5.0f);
                    velocity.z = test_rand_range(&rng, -5.0f, 5.0f);
                    ASSERT_STATUS(
                        lt_add_component(world, states[idx].entity, velocity_id, &velocity),
                        LT_STATUS_OK);
                    states[idx].has_velocity = 1u;
                }
            } else {
                ASSERT_STATUS(lt_entity_destroy(world, states[idx].entity), LT_STATUS_OK);
                states[idx].alive = 0u;
                states[idx].has_position = 0u;
                states[idx].has_velocity = 0u;
            }
        }

        ASSERT_STATUS(lt_world_end_defer(world), LT_STATUS_OK);
        ASSERT_STATUS(lt_world_flush(world), LT_STATUS_OK);

        ASSERT_STATUS(lt_world_get_stats(world, &stats), LT_STATUS_OK);
        checksum = test_checksum_mix(checksum, stats.live_entities);
        checksum = test_checksum_mix(checksum, stats.chunk_count);
        checksum = test_checksum_mix(checksum, stats.structural_moves);

        {
            uint32_t tracked_alive_count;

            tracked_alive_count = 0u;
            for (i = 0u; i < state_count; ++i) {
                uint8_t alive;

                ASSERT_STATUS(lt_entity_is_alive(world, states[i].entity, &alive), LT_STATUS_OK);
                ASSERT_TRUE(alive == states[i].alive);
                if (states[i].alive == 0u) {
                    continue;
                }

                {
                    uint8_t has_position;
                    uint8_t has_velocity;

                    ASSERT_STATUS(
                        lt_has_component(world, states[i].entity, position_id, &has_position),
                        LT_STATUS_OK);
                    ASSERT_STATUS(
                        lt_has_component(world, states[i].entity, velocity_id, &has_velocity),
                        LT_STATUS_OK);
                    ASSERT_TRUE(has_position == states[i].has_position);
                    ASSERT_TRUE(has_velocity == states[i].has_velocity);
                }

                tracked_alive_count += 1u;
            }

            ASSERT_TRUE(stats.live_entities == tracked_alive_count);
            out_snapshot->tracked_alive_count = tracked_alive_count;
        }
    }

    ASSERT_STATUS(lt_world_get_stats(world, &out_snapshot->stats), LT_STATUS_OK);
    out_snapshot->checksum = checksum;
    lt_query_destroy(query);
    lt_world_destroy(world);
    return 0;
}

static int test_world_create_destroy_defaults(void)
{
    lt_world_t* world;
    lt_world_stats_t stats;

    ASSERT_STATUS(lt_world_create(NULL, &world), LT_STATUS_OK);
    ASSERT_TRUE(world != NULL);

    ASSERT_STATUS(lt_world_get_stats(world, &stats), LT_STATUS_OK);
    ASSERT_TRUE(stats.live_entities == 0u);
    ASSERT_TRUE(stats.registered_components == 0u);
    ASSERT_TRUE(stats.archetype_count >= 1u);
    ASSERT_TRUE(stats.structural_moves == 0ull);

    lt_world_destroy(world);
    return 0;
}

static int test_world_rejects_partial_allocator_config(void)
{
    lt_world_t* world;
    lt_world_config_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.allocator.alloc = test_alloc_only;

    ASSERT_STATUS(lt_world_create(&cfg, &world), LT_STATUS_INVALID_ARGUMENT);
    return 0;
}

static int test_entity_lifecycle_and_stale_generation(void)
{
    lt_world_t* world;
    lt_entity_t e0;
    lt_entity_t e1;
    lt_world_stats_t stats;
    uint8_t alive;

    ASSERT_STATUS(lt_world_create(NULL, &world), LT_STATUS_OK);

    ASSERT_STATUS(lt_entity_create(world, &e0), LT_STATUS_OK);
    ASSERT_STATUS(lt_entity_is_alive(world, e0, &alive), LT_STATUS_OK);
    ASSERT_TRUE(alive == 1u);

    ASSERT_STATUS(lt_entity_destroy(world, e0), LT_STATUS_OK);
    ASSERT_STATUS(lt_entity_is_alive(world, e0, &alive), LT_STATUS_OK);
    ASSERT_TRUE(alive == 0u);
    ASSERT_STATUS(lt_entity_destroy(world, e0), LT_STATUS_STALE_ENTITY);

    ASSERT_STATUS(lt_entity_create(world, &e1), LT_STATUS_OK);
    ASSERT_TRUE(e1 != e0);

    ASSERT_STATUS(lt_world_get_stats(world, &stats), LT_STATUS_OK);
    ASSERT_TRUE(stats.live_entities == 1u);
    ASSERT_TRUE(stats.free_entity_slots == 0u);

    lt_world_destroy(world);
    return 0;
}

static int test_entity_capacity_growth(void)
{
    enum { ENTITY_COUNT = 300 };
    lt_world_t* world;
    lt_world_config_t cfg;
    lt_entity_t entities[ENTITY_COUNT];
    lt_world_stats_t stats;
    int i;

    memset(&cfg, 0, sizeof(cfg));
    cfg.initial_entity_capacity = 4u;

    ASSERT_STATUS(lt_world_create(&cfg, &world), LT_STATUS_OK);

    for (i = 0; i < ENTITY_COUNT; ++i) {
        ASSERT_STATUS(lt_entity_create(world, &entities[i]), LT_STATUS_OK);
        ASSERT_TRUE(entities[i] != LT_ENTITY_NULL);
    }

    ASSERT_STATUS(lt_world_get_stats(world, &stats), LT_STATUS_OK);
    ASSERT_TRUE(stats.live_entities == ENTITY_COUNT);
    ASSERT_TRUE(stats.entity_capacity >= ENTITY_COUNT);

    lt_world_destroy(world);
    return 0;
}

static int test_component_registration(void)
{
    lt_world_t* world;
    lt_component_desc_t desc;
    lt_component_id_t c0;
    lt_component_id_t c1;

    ASSERT_STATUS(lt_world_create(NULL, &world), LT_STATUS_OK);

    memset(&desc, 0, sizeof(desc));
    desc.name = "Transform";
    desc.size = 16u;
    desc.align = 8u;

    ASSERT_STATUS(lt_register_component(world, &desc, &c0), LT_STATUS_OK);
    ASSERT_TRUE(c0 != LT_COMPONENT_INVALID);

    ASSERT_STATUS(lt_register_component(world, &desc, &c1), LT_STATUS_ALREADY_EXISTS);

    desc.name = "Velocity";
    ASSERT_STATUS(lt_register_component(world, &desc, &c1), LT_STATUS_OK);
    ASSERT_TRUE(c1 == c0 + 1u);

    lt_world_destroy(world);
    return 0;
}

static int test_component_validation(void)
{
    lt_world_t* world;
    lt_component_desc_t desc;
    lt_component_id_t id;

    ASSERT_STATUS(lt_world_create(NULL, &world), LT_STATUS_OK);

    memset(&desc, 0, sizeof(desc));
    desc.name = "BadAlign";
    desc.size = 8u;
    desc.align = 3u;
    ASSERT_STATUS(lt_register_component(world, &desc, &id), LT_STATUS_INVALID_ARGUMENT);

    memset(&desc, 0, sizeof(desc));
    desc.name = "Tag";
    desc.flags = LT_COMPONENT_FLAG_TAG;
    desc.size = 0u;
    desc.align = 1u;
    ASSERT_STATUS(lt_register_component(world, &desc, &id), LT_STATUS_OK);

    memset(&desc, 0, sizeof(desc));
    desc.name = "TagWithSize";
    desc.flags = LT_COMPONENT_FLAG_TAG;
    desc.size = 4u;
    desc.align = 1u;
    ASSERT_STATUS(lt_register_component(world, &desc, &id), LT_STATUS_INVALID_ARGUMENT);

    lt_world_destroy(world);
    return 0;
}

static int test_add_remove_components_preserve_data(void)
{
    lt_world_t* world;
    lt_component_id_t position_id;
    lt_component_id_t velocity_id;
    lt_entity_t entity;
    test_vec3_t position;
    test_vec3_t velocity;
    test_vec3_t* position_ptr;
    test_vec3_t* velocity_ptr;
    uint8_t has;

    ASSERT_STATUS(lt_world_create(NULL, &world), LT_STATUS_OK);
    ASSERT_TRUE(register_vec3_components(world, &position_id, &velocity_id) == 0);

    ASSERT_STATUS(lt_entity_create(world, &entity), LT_STATUS_OK);

    position.x = 1.0f;
    position.y = 2.0f;
    position.z = 3.0f;
    ASSERT_STATUS(lt_add_component(world, entity, position_id, &position), LT_STATUS_OK);

    position_ptr = NULL;
    ASSERT_STATUS(lt_get_component(world, entity, position_id, (void**)&position_ptr), LT_STATUS_OK);
    ASSERT_TRUE(position_ptr->x == 1.0f);
    ASSERT_TRUE(position_ptr->y == 2.0f);
    ASSERT_TRUE(position_ptr->z == 3.0f);

    velocity.x = 4.0f;
    velocity.y = 5.0f;
    velocity.z = 6.0f;
    ASSERT_STATUS(lt_add_component(world, entity, velocity_id, &velocity), LT_STATUS_OK);

    position_ptr = NULL;
    velocity_ptr = NULL;
    ASSERT_STATUS(lt_get_component(world, entity, position_id, (void**)&position_ptr), LT_STATUS_OK);
    ASSERT_STATUS(lt_get_component(world, entity, velocity_id, (void**)&velocity_ptr), LT_STATUS_OK);
    ASSERT_TRUE(position_ptr->x == 1.0f);
    ASSERT_TRUE(position_ptr->y == 2.0f);
    ASSERT_TRUE(position_ptr->z == 3.0f);
    ASSERT_TRUE(velocity_ptr->x == 4.0f);
    ASSERT_TRUE(velocity_ptr->y == 5.0f);
    ASSERT_TRUE(velocity_ptr->z == 6.0f);

    ASSERT_STATUS(lt_add_component(world, entity, velocity_id, &velocity), LT_STATUS_ALREADY_EXISTS);

    ASSERT_STATUS(lt_remove_component(world, entity, position_id), LT_STATUS_OK);

    ASSERT_STATUS(lt_has_component(world, entity, position_id, &has), LT_STATUS_OK);
    ASSERT_TRUE(has == 0u);
    ASSERT_STATUS(lt_has_component(world, entity, velocity_id, &has), LT_STATUS_OK);
    ASSERT_TRUE(has == 1u);

    velocity_ptr = NULL;
    ASSERT_STATUS(lt_get_component(world, entity, velocity_id, (void**)&velocity_ptr), LT_STATUS_OK);
    ASSERT_TRUE(velocity_ptr->x == 4.0f);
    ASSERT_TRUE(velocity_ptr->y == 5.0f);
    ASSERT_TRUE(velocity_ptr->z == 6.0f);

    ASSERT_STATUS(lt_remove_component(world, entity, position_id), LT_STATUS_NOT_FOUND);

    lt_world_destroy(world);
    return 0;
}

static int test_swap_remove_updates_entity_locations(void)
{
    lt_world_t* world;
    lt_component_id_t position_id;
    lt_component_id_t velocity_id;
    lt_entity_t a;
    lt_entity_t b;
    test_vec3_t pa;
    test_vec3_t pb;
    test_vec3_t* out_b;

    ASSERT_STATUS(lt_world_create(NULL, &world), LT_STATUS_OK);
    ASSERT_TRUE(register_vec3_components(world, &position_id, &velocity_id) == 0);

    ASSERT_STATUS(lt_entity_create(world, &a), LT_STATUS_OK);
    ASSERT_STATUS(lt_entity_create(world, &b), LT_STATUS_OK);

    pa.x = 11.0f;
    pa.y = 12.0f;
    pa.z = 13.0f;
    pb.x = 21.0f;
    pb.y = 22.0f;
    pb.z = 23.0f;

    ASSERT_STATUS(lt_add_component(world, a, position_id, &pa), LT_STATUS_OK);
    ASSERT_STATUS(lt_add_component(world, b, position_id, &pb), LT_STATUS_OK);

    ASSERT_STATUS(lt_remove_component(world, a, position_id), LT_STATUS_OK);

    out_b = NULL;
    ASSERT_STATUS(lt_get_component(world, b, position_id, (void**)&out_b), LT_STATUS_OK);
    ASSERT_TRUE(out_b->x == 21.0f);
    ASSERT_TRUE(out_b->y == 22.0f);
    ASSERT_TRUE(out_b->z == 23.0f);

    lt_world_destroy(world);
    return 0;
}

static int test_world_stats_structural_moves(void)
{
    lt_world_t* world;
    lt_component_id_t position_id;
    lt_component_id_t velocity_id;
    lt_entity_t a;
    lt_entity_t b;
    lt_world_stats_t stats;
    test_vec3_t position;

    ASSERT_STATUS(lt_world_create(NULL, &world), LT_STATUS_OK);
    ASSERT_TRUE(register_vec3_components(world, &position_id, &velocity_id) == 0);

    ASSERT_STATUS(lt_world_get_stats(world, &stats), LT_STATUS_OK);
    ASSERT_TRUE(stats.structural_moves == 0ull);

    ASSERT_STATUS(lt_entity_create(world, &a), LT_STATUS_OK);
    ASSERT_STATUS(lt_entity_create(world, &b), LT_STATUS_OK);

    position.x = 1.0f;
    position.y = 2.0f;
    position.z = 3.0f;

    ASSERT_STATUS(lt_add_component(world, a, position_id, &position), LT_STATUS_OK);
    ASSERT_STATUS(lt_world_get_stats(world, &stats), LT_STATUS_OK);
    ASSERT_TRUE(stats.structural_moves == 2ull);

    ASSERT_STATUS(lt_add_component(world, b, position_id, &position), LT_STATUS_OK);
    ASSERT_STATUS(lt_world_get_stats(world, &stats), LT_STATUS_OK);
    ASSERT_TRUE(stats.structural_moves == 3ull);

    ASSERT_STATUS(lt_remove_component(world, a, position_id), LT_STATUS_OK);
    ASSERT_STATUS(lt_world_get_stats(world, &stats), LT_STATUS_OK);
    ASSERT_TRUE(stats.structural_moves == 5ull);

    ASSERT_STATUS(lt_remove_component(world, b, position_id), LT_STATUS_OK);
    ASSERT_STATUS(lt_world_get_stats(world, &stats), LT_STATUS_OK);
    ASSERT_TRUE(stats.structural_moves == 6ull);

    lt_world_destroy(world);
    return 0;
}

static int test_destructors_called_on_remove_destroy_and_world_destroy(void)
{
    lt_world_t* world;
    lt_component_desc_t desc;
    lt_component_id_t resource_id;
    lt_entity_t e0;
    lt_entity_t e1;
    uint32_t value;
    int dtor_calls;

    ASSERT_STATUS(lt_world_create(NULL, &world), LT_STATUS_OK);

    dtor_calls = 0;
    memset(&desc, 0, sizeof(desc));
    desc.name = "Resource";
    desc.size = (uint32_t)sizeof(uint32_t);
    desc.align = (uint32_t)_Alignof(uint32_t);
    desc.dtor = test_counting_dtor;
    desc.user = &dtor_calls;
    ASSERT_STATUS(lt_register_component(world, &desc, &resource_id), LT_STATUS_OK);

    value = 42u;
    ASSERT_STATUS(lt_entity_create(world, &e0), LT_STATUS_OK);
    ASSERT_STATUS(lt_add_component(world, e0, resource_id, &value), LT_STATUS_OK);
    ASSERT_STATUS(lt_remove_component(world, e0, resource_id), LT_STATUS_OK);
    ASSERT_TRUE(dtor_calls == 1);

    ASSERT_STATUS(lt_add_component(world, e0, resource_id, &value), LT_STATUS_OK);
    ASSERT_STATUS(lt_entity_destroy(world, e0), LT_STATUS_OK);
    ASSERT_TRUE(dtor_calls == 2);

    ASSERT_STATUS(lt_entity_create(world, &e1), LT_STATUS_OK);
    ASSERT_STATUS(lt_add_component(world, e1, resource_id, &value), LT_STATUS_OK);

    lt_world_destroy(world);
    ASSERT_TRUE(dtor_calls == 3);
    return 0;
}

static int test_tag_component_behavior(void)
{
    lt_world_t* world;
    lt_component_desc_t desc;
    lt_component_id_t tag_id;
    lt_entity_t entity;
    uint8_t has;
    void* ptr;

    ASSERT_STATUS(lt_world_create(NULL, &world), LT_STATUS_OK);

    memset(&desc, 0, sizeof(desc));
    desc.name = "EnemyTag";
    desc.flags = LT_COMPONENT_FLAG_TAG;
    desc.size = 0u;
    desc.align = 1u;
    ASSERT_STATUS(lt_register_component(world, &desc, &tag_id), LT_STATUS_OK);

    ASSERT_STATUS(lt_entity_create(world, &entity), LT_STATUS_OK);
    ASSERT_STATUS(lt_add_component(world, entity, tag_id, NULL), LT_STATUS_OK);

    ASSERT_STATUS(lt_has_component(world, entity, tag_id, &has), LT_STATUS_OK);
    ASSERT_TRUE(has == 1u);

    ptr = (void*)0x1;
    ASSERT_STATUS(lt_get_component(world, entity, tag_id, &ptr), LT_STATUS_OK);
    ASSERT_TRUE(ptr == NULL);

    ASSERT_STATUS(lt_remove_component(world, entity, tag_id), LT_STATUS_OK);
    ASSERT_STATUS(lt_has_component(world, entity, tag_id, &has), LT_STATUS_OK);
    ASSERT_TRUE(has == 0u);

    lt_world_destroy(world);
    return 0;
}

static int test_query_iteration_and_filters(void)
{
    lt_world_t* world;
    lt_component_id_t position_id;
    lt_component_id_t velocity_id;
    lt_entity_t e0;
    lt_entity_t e1;
    lt_entity_t e2;
    lt_entity_t e3;
    test_vec3_t position;
    test_vec3_t velocity;
    lt_query_term_t pos_only_term;
    lt_query_term_t movable_terms[2];
    lt_query_desc_t pos_only_desc;
    lt_query_desc_t movable_desc;
    lt_query_t* pos_only_query;
    lt_query_t* movable_query;
    lt_query_iter_t iter;
    lt_chunk_view_t view;
    uint8_t has_value;
    uint8_t has_pos;
    uint8_t has_vel;
    uint32_t count;
    uint8_t saw_e0;
    uint8_t saw_e1;
    uint8_t saw_e2;

    ASSERT_STATUS(lt_world_create(NULL, &world), LT_STATUS_OK);
    ASSERT_TRUE(register_vec3_components(world, &position_id, &velocity_id) == 0);

    ASSERT_STATUS(lt_entity_create(world, &e0), LT_STATUS_OK);
    ASSERT_STATUS(lt_entity_create(world, &e1), LT_STATUS_OK);
    ASSERT_STATUS(lt_entity_create(world, &e2), LT_STATUS_OK);
    ASSERT_STATUS(lt_entity_create(world, &e3), LT_STATUS_OK);

    position.x = 1.0f;
    position.y = 2.0f;
    position.z = 3.0f;
    velocity.x = 7.0f;
    velocity.y = 8.0f;
    velocity.z = 9.0f;

    ASSERT_STATUS(lt_add_component(world, e0, position_id, &position), LT_STATUS_OK);
    ASSERT_STATUS(lt_add_component(world, e1, position_id, &position), LT_STATUS_OK);
    ASSERT_STATUS(lt_add_component(world, e1, velocity_id, &velocity), LT_STATUS_OK);
    ASSERT_STATUS(lt_add_component(world, e2, velocity_id, &velocity), LT_STATUS_OK);

    memset(&pos_only_term, 0, sizeof(pos_only_term));
    pos_only_term.component_id = position_id;
    pos_only_term.access = LT_ACCESS_READ;

    memset(&pos_only_desc, 0, sizeof(pos_only_desc));
    pos_only_desc.with_terms = &pos_only_term;
    pos_only_desc.with_count = 1u;
    pos_only_desc.without = &velocity_id;
    pos_only_desc.without_count = 1u;

    movable_terms[0].component_id = position_id;
    movable_terms[0].access = LT_ACCESS_WRITE;
    movable_terms[1].component_id = velocity_id;
    movable_terms[1].access = LT_ACCESS_READ;

    memset(&movable_desc, 0, sizeof(movable_desc));
    movable_desc.with_terms = movable_terms;
    movable_desc.with_count = 2u;

    ASSERT_STATUS(lt_query_create(world, &pos_only_desc, &pos_only_query), LT_STATUS_OK);
    ASSERT_STATUS(lt_query_create(world, &movable_desc, &movable_query), LT_STATUS_OK);

    ASSERT_STATUS(lt_query_iter_begin(pos_only_query, &iter), LT_STATUS_OK);
    count = 0u;
    saw_e0 = 0u;
    while (1) {
        ASSERT_STATUS(lt_query_iter_next(&iter, &view, &has_value), LT_STATUS_OK);
        if (has_value == 0u) {
            break;
        }

        for (uint32_t i = 0u; i < view.count; ++i) {
            lt_entity_t entity;
            entity = view.entities[i];
            count += 1u;
            if (entity == e0) {
                saw_e0 = 1u;
            }
            ASSERT_STATUS(lt_has_component(world, entity, position_id, &has_pos), LT_STATUS_OK);
            ASSERT_STATUS(lt_has_component(world, entity, velocity_id, &has_vel), LT_STATUS_OK);
            ASSERT_TRUE(has_pos == 1u);
            ASSERT_TRUE(has_vel == 0u);
        }
    }
    ASSERT_TRUE(count == 1u);
    ASSERT_TRUE(saw_e0 == 1u);

    ASSERT_STATUS(lt_query_iter_begin(movable_query, &iter), LT_STATUS_OK);
    count = 0u;
    saw_e1 = 0u;
    while (1) {
        ASSERT_STATUS(lt_query_iter_next(&iter, &view, &has_value), LT_STATUS_OK);
        if (has_value == 0u) {
            break;
        }

        for (uint32_t i = 0u; i < view.count; ++i) {
            lt_entity_t entity;
            entity = view.entities[i];
            count += 1u;
            if (entity == e1) {
                saw_e1 = 1u;
            }
            ASSERT_STATUS(lt_has_component(world, entity, position_id, &has_pos), LT_STATUS_OK);
            ASSERT_STATUS(lt_has_component(world, entity, velocity_id, &has_vel), LT_STATUS_OK);
            ASSERT_TRUE(has_pos == 1u);
            ASSERT_TRUE(has_vel == 1u);
        }
    }
    ASSERT_TRUE(count == 1u);
    ASSERT_TRUE(saw_e1 == 1u);

    ASSERT_STATUS(lt_add_component(world, e0, velocity_id, &velocity), LT_STATUS_OK);

    ASSERT_STATUS(lt_query_iter_begin(movable_query, &iter), LT_STATUS_OK);
    count = 0u;
    saw_e0 = 0u;
    saw_e1 = 0u;
    saw_e2 = 0u;
    while (1) {
        ASSERT_STATUS(lt_query_iter_next(&iter, &view, &has_value), LT_STATUS_OK);
        if (has_value == 0u) {
            break;
        }

        for (uint32_t i = 0u; i < view.count; ++i) {
            lt_entity_t entity;
            entity = view.entities[i];
            count += 1u;
            if (entity == e0) {
                saw_e0 = 1u;
            } else if (entity == e1) {
                saw_e1 = 1u;
            } else if (entity == e2) {
                saw_e2 = 1u;
            }
        }
    }
    ASSERT_TRUE(count == 2u);
    ASSERT_TRUE(saw_e0 == 1u);
    ASSERT_TRUE(saw_e1 == 1u);
    ASSERT_TRUE(saw_e2 == 0u);

    lt_query_destroy(pos_only_query);
    lt_query_destroy(movable_query);
    lt_world_destroy(world);
    return 0;
}

static int test_query_validation_conflicts(void)
{
    lt_world_t* world;
    lt_component_id_t position_id;
    lt_component_id_t velocity_id;
    lt_query_term_t with_term;
    lt_query_desc_t desc;
    lt_query_t* query;

    ASSERT_STATUS(lt_world_create(NULL, &world), LT_STATUS_OK);
    ASSERT_TRUE(register_vec3_components(world, &position_id, &velocity_id) == 0);

    with_term.component_id = position_id;
    with_term.access = LT_ACCESS_READ;

    memset(&desc, 0, sizeof(desc));
    desc.with_terms = &with_term;
    desc.with_count = 1u;
    desc.without = &position_id;
    desc.without_count = 1u;
    ASSERT_STATUS(lt_query_create(world, &desc, &query), LT_STATUS_CONFLICT);

    with_term.component_id = LT_COMPONENT_INVALID;
    memset(&desc, 0, sizeof(desc));
    desc.with_terms = &with_term;
    desc.with_count = 1u;
    ASSERT_STATUS(lt_query_create(world, &desc, &query), LT_STATUS_NOT_FOUND);

    lt_world_destroy(world);
    return 0;
}

static int test_deferred_component_visibility_and_payload_copy(void)
{
    lt_world_t* world;
    lt_component_id_t position_id;
    lt_component_id_t velocity_id;
    lt_entity_t entity;
    test_vec3_t position;
    test_vec3_t* out_position;
    lt_world_stats_t stats;
    uint8_t has;

    ASSERT_STATUS(lt_world_create(NULL, &world), LT_STATUS_OK);
    ASSERT_TRUE(register_vec3_components(world, &position_id, &velocity_id) == 0);
    ASSERT_STATUS(lt_entity_create(world, &entity), LT_STATUS_OK);

    position.x = 3.0f;
    position.y = 4.0f;
    position.z = 5.0f;

    ASSERT_STATUS(lt_world_begin_defer(world), LT_STATUS_OK);
    ASSERT_STATUS(lt_add_component(world, entity, position_id, &position), LT_STATUS_OK);

    position.x = 99.0f;
    position.y = 100.0f;
    position.z = 101.0f;

    ASSERT_STATUS(lt_has_component(world, entity, position_id, &has), LT_STATUS_OK);
    ASSERT_TRUE(has == 0u);

    ASSERT_STATUS(lt_world_get_stats(world, &stats), LT_STATUS_OK);
    ASSERT_TRUE(stats.pending_commands == 1u);
    ASSERT_TRUE(stats.defer_depth == 1u);

    ASSERT_STATUS(lt_world_end_defer(world), LT_STATUS_OK);
    ASSERT_STATUS(lt_world_flush(world), LT_STATUS_OK);

    ASSERT_STATUS(lt_has_component(world, entity, position_id, &has), LT_STATUS_OK);
    ASSERT_TRUE(has == 1u);

    out_position = NULL;
    ASSERT_STATUS(lt_get_component(world, entity, position_id, (void**)&out_position), LT_STATUS_OK);
    ASSERT_TRUE(out_position->x == 3.0f);
    ASSERT_TRUE(out_position->y == 4.0f);
    ASSERT_TRUE(out_position->z == 5.0f);

    ASSERT_STATUS(lt_world_get_stats(world, &stats), LT_STATUS_OK);
    ASSERT_TRUE(stats.pending_commands == 0u);
    ASSERT_TRUE(stats.defer_depth == 0u);

    lt_world_destroy(world);
    return 0;
}

static int test_deferred_flush_conflict_and_destroy(void)
{
    lt_world_t* world;
    lt_entity_t entity;
    uint8_t alive;

    ASSERT_STATUS(lt_world_create(NULL, &world), LT_STATUS_OK);
    ASSERT_STATUS(lt_entity_create(world, &entity), LT_STATUS_OK);

    ASSERT_STATUS(lt_world_begin_defer(world), LT_STATUS_OK);
    ASSERT_STATUS(lt_world_begin_defer(world), LT_STATUS_OK);
    ASSERT_STATUS(lt_entity_destroy(world, entity), LT_STATUS_OK);

    ASSERT_STATUS(lt_world_flush(world), LT_STATUS_CONFLICT);
    ASSERT_STATUS(lt_world_end_defer(world), LT_STATUS_OK);
    ASSERT_STATUS(lt_world_flush(world), LT_STATUS_CONFLICT);
    ASSERT_STATUS(lt_world_end_defer(world), LT_STATUS_OK);
    ASSERT_STATUS(lt_world_flush(world), LT_STATUS_OK);

    ASSERT_STATUS(lt_entity_is_alive(world, entity, &alive), LT_STATUS_OK);
    ASSERT_TRUE(alive == 0u);

    lt_world_destroy(world);
    return 0;
}

static int test_deferred_command_ordering(void)
{
    lt_world_t* world;
    lt_component_id_t position_id;
    lt_component_id_t velocity_id;
    lt_entity_t entity;
    test_vec3_t p0;
    test_vec3_t p1;
    test_vec3_t* out_position;
    uint8_t has;

    ASSERT_STATUS(lt_world_create(NULL, &world), LT_STATUS_OK);
    ASSERT_TRUE(register_vec3_components(world, &position_id, &velocity_id) == 0);
    ASSERT_STATUS(lt_entity_create(world, &entity), LT_STATUS_OK);

    p0.x = 1.0f;
    p0.y = 1.0f;
    p0.z = 1.0f;
    p1.x = 2.0f;
    p1.y = 2.0f;
    p1.z = 2.0f;

    ASSERT_STATUS(lt_world_begin_defer(world), LT_STATUS_OK);
    ASSERT_STATUS(lt_add_component(world, entity, position_id, &p0), LT_STATUS_OK);
    ASSERT_STATUS(lt_remove_component(world, entity, position_id), LT_STATUS_OK);
    ASSERT_STATUS(lt_add_component(world, entity, position_id, &p1), LT_STATUS_OK);
    ASSERT_STATUS(lt_world_end_defer(world), LT_STATUS_OK);
    ASSERT_STATUS(lt_world_flush(world), LT_STATUS_OK);

    ASSERT_STATUS(lt_has_component(world, entity, position_id, &has), LT_STATUS_OK);
    ASSERT_TRUE(has == 1u);

    out_position = NULL;
    ASSERT_STATUS(lt_get_component(world, entity, position_id, (void**)&out_position), LT_STATUS_OK);
    ASSERT_TRUE(out_position->x == 2.0f);
    ASSERT_TRUE(out_position->y == 2.0f);
    ASSERT_TRUE(out_position->z == 2.0f);

    lt_world_destroy(world);
    return 0;
}

static int test_trace_hook_reports_core_events(void)
{
    lt_world_t* world;
    lt_component_id_t position_id;
    lt_component_id_t velocity_id;
    lt_entity_t entity;
    test_vec3_t position;
    test_trace_capture_t capture;

    ASSERT_STATUS(lt_world_create(NULL, &world), LT_STATUS_OK);
    ASSERT_TRUE(register_vec3_components(world, &position_id, &velocity_id) == 0);

    memset(&capture, 0, sizeof(capture));
    ASSERT_STATUS(lt_world_set_trace_hook(world, test_trace_hook, &capture), LT_STATUS_OK);

    ASSERT_STATUS(lt_entity_create(world, &entity), LT_STATUS_OK);

    position.x = 1.0f;
    position.y = 2.0f;
    position.z = 3.0f;

    ASSERT_STATUS(lt_world_begin_defer(world), LT_STATUS_OK);
    ASSERT_STATUS(lt_add_component(world, entity, position_id, &position), LT_STATUS_OK);
    ASSERT_STATUS(lt_world_end_defer(world), LT_STATUS_OK);
    ASSERT_STATUS(lt_world_flush(world), LT_STATUS_OK);
    ASSERT_STATUS(lt_remove_component(world, entity, position_id), LT_STATUS_OK);
    ASSERT_STATUS(lt_entity_destroy(world, entity), LT_STATUS_OK);

    ASSERT_TRUE(capture.total > 0u);
    ASSERT_TRUE(capture.entity_create_count >= 1u);
    ASSERT_TRUE(capture.defer_begin_count == 1u);
    ASSERT_TRUE(capture.defer_end_count == 1u);
    ASSERT_TRUE(capture.defer_enqueue_count >= 1u);
    ASSERT_TRUE(capture.flush_begin_count == 1u);
    ASSERT_TRUE(capture.flush_apply_count >= 1u);
    ASSERT_TRUE(capture.flush_end_count == 1u);
    ASSERT_TRUE(capture.component_add_count >= 1u);
    ASSERT_TRUE(capture.component_remove_count >= 1u);
    ASSERT_TRUE(capture.entity_destroy_count >= 1u);
    ASSERT_TRUE(capture.last_status == LT_STATUS_OK);

    ASSERT_STATUS(lt_world_set_trace_hook(world, NULL, NULL), LT_STATUS_OK);
    lt_world_destroy(world);
    return 0;
}

static int test_trace_hook_reports_query_events(void)
{
    lt_world_t* world;
    lt_component_id_t position_id;
    lt_component_id_t velocity_id;
    lt_entity_t entity;
    test_vec3_t position;
    test_vec3_t velocity;
    lt_query_term_t terms[2];
    lt_query_desc_t query_desc;
    lt_query_t* query;
    lt_query_iter_t iter;
    lt_chunk_view_t view;
    uint8_t has_value;
    test_trace_capture_t capture;

    ASSERT_STATUS(lt_world_create(NULL, &world), LT_STATUS_OK);
    ASSERT_TRUE(register_vec3_components(world, &position_id, &velocity_id) == 0);

    memset(&capture, 0, sizeof(capture));
    ASSERT_STATUS(lt_world_set_trace_hook(world, test_trace_hook, &capture), LT_STATUS_OK);

    ASSERT_STATUS(lt_entity_create(world, &entity), LT_STATUS_OK);

    position.x = 1.0f;
    position.y = 2.0f;
    position.z = 3.0f;
    velocity.x = 0.25f;
    velocity.y = 0.5f;
    velocity.z = 0.75f;

    ASSERT_STATUS(lt_add_component(world, entity, position_id, &position), LT_STATUS_OK);
    ASSERT_STATUS(lt_add_component(world, entity, velocity_id, &velocity), LT_STATUS_OK);

    memset(terms, 0, sizeof(terms));
    terms[0].component_id = position_id;
    terms[0].access = LT_ACCESS_WRITE;
    terms[1].component_id = velocity_id;
    terms[1].access = LT_ACCESS_READ;

    memset(&query_desc, 0, sizeof(query_desc));
    query_desc.with_terms = terms;
    query_desc.with_count = 2u;

    ASSERT_STATUS(lt_query_create(world, &query_desc, &query), LT_STATUS_OK);
    ASSERT_STATUS(lt_query_iter_begin(query, &iter), LT_STATUS_OK);

    while (1) {
        ASSERT_STATUS(lt_query_iter_next(&iter, &view, &has_value), LT_STATUS_OK);
        if (has_value == 0u) {
            break;
        }
        ASSERT_TRUE(view.count > 0u);
    }

    ASSERT_STATUS(lt_query_iter_next(&iter, &view, &has_value), LT_STATUS_OK);
    ASSERT_TRUE(has_value == 0u);

    ASSERT_TRUE(capture.query_begin_count == 1u);
    ASSERT_TRUE(capture.query_chunk_count >= 1u);
    ASSERT_TRUE(capture.query_end_count == 1u);
    ASSERT_TRUE(capture.last_kind == LT_TRACE_EVENT_QUERY_ITER_END);
    ASSERT_TRUE(capture.last_status == LT_STATUS_OK);

    lt_query_destroy(query);
    lt_world_destroy(world);
    return 0;
}

static int test_determinism_seeded_mixed_sequence(void)
{
    test_determinism_snapshot_t run_a;
    test_determinism_snapshot_t run_b;
    test_determinism_snapshot_t run_c;

    ASSERT_TRUE(run_seeded_determinism_sequence(0x00C0FFEEu, &run_a) == 0);
    ASSERT_TRUE(run_seeded_determinism_sequence(0x00C0FFEEu, &run_b) == 0);

    ASSERT_TRUE(run_a.checksum == run_b.checksum);
    ASSERT_TRUE(run_a.tracked_alive_count == run_b.tracked_alive_count);
    ASSERT_TRUE(run_a.stats.live_entities == run_b.stats.live_entities);
    ASSERT_TRUE(run_a.stats.archetype_count == run_b.stats.archetype_count);
    ASSERT_TRUE(run_a.stats.chunk_count == run_b.stats.chunk_count);
    ASSERT_TRUE(run_a.stats.pending_commands == run_b.stats.pending_commands);
    ASSERT_TRUE(run_a.stats.defer_depth == run_b.stats.defer_depth);
    ASSERT_TRUE(run_a.stats.structural_moves == run_b.stats.structural_moves);

    ASSERT_TRUE(run_seeded_determinism_sequence(0x00C0FFEFu, &run_c) == 0);
    ASSERT_TRUE(
        run_c.checksum != run_a.checksum || run_c.stats.structural_moves != run_a.stats.structural_moves
        || run_c.stats.live_entities != run_a.stats.live_entities);
    return 0;
}

int main(void)
{
    RUN_TEST(test_world_create_destroy_defaults);
    RUN_TEST(test_world_rejects_partial_allocator_config);
    RUN_TEST(test_entity_lifecycle_and_stale_generation);
    RUN_TEST(test_entity_capacity_growth);
    RUN_TEST(test_component_registration);
    RUN_TEST(test_component_validation);
    RUN_TEST(test_add_remove_components_preserve_data);
    RUN_TEST(test_swap_remove_updates_entity_locations);
    RUN_TEST(test_world_stats_structural_moves);
    RUN_TEST(test_destructors_called_on_remove_destroy_and_world_destroy);
    RUN_TEST(test_tag_component_behavior);
    RUN_TEST(test_query_iteration_and_filters);
    RUN_TEST(test_query_validation_conflicts);
    RUN_TEST(test_deferred_component_visibility_and_payload_copy);
    RUN_TEST(test_deferred_flush_conflict_and_destroy);
    RUN_TEST(test_deferred_command_ordering);
    RUN_TEST(test_trace_hook_reports_core_events);
    RUN_TEST(test_trace_hook_reports_query_events);
    RUN_TEST(test_determinism_seeded_mixed_sequence);
    return 0;
}
