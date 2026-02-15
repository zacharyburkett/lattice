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
    RUN_TEST(test_destructors_called_on_remove_destroy_and_world_destroy);
    RUN_TEST(test_tag_component_behavior);
    return 0;
}
