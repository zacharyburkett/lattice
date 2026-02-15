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

static void* test_alloc_only(void* user, size_t size, size_t align)
{
    (void)user;
    (void)size;
    (void)align;
    return NULL;
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

int main(void)
{
    RUN_TEST(test_world_create_destroy_defaults);
    RUN_TEST(test_world_rejects_partial_allocator_config);
    RUN_TEST(test_entity_lifecycle_and_stale_generation);
    RUN_TEST(test_entity_capacity_growth);
    RUN_TEST(test_component_registration);
    RUN_TEST(test_component_validation);
    return 0;
}
