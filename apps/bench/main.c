#include "lattice/lattice.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct bench_vec3_s {
    float x;
    float y;
    float z;
} bench_vec3_t;

typedef struct bench_health_s {
    float value;
} bench_health_t;

typedef struct bench_churn_s {
    float resistance;
} bench_churn_t;

typedef enum bench_output_format_e {
    BENCH_OUTPUT_TEXT = 0,
    BENCH_OUTPUT_CSV = 1,
    BENCH_OUTPUT_JSON = 2
} bench_output_format_t;

typedef enum bench_scene_e {
    BENCH_SCENE_STEADY = 0,
    BENCH_SCENE_CHURN = 1
} bench_scene_t;

enum {
    BENCH_SWEEP_WORKER_COUNT_DEFAULT = 4,
    BENCH_SWEEP_WORKER_COUNT_MAX = 16
};

typedef struct bench_options_s {
    uint32_t entity_count;
    uint32_t frame_count;
    uint32_t seed;
    uint8_t use_defer;
    bench_output_format_t output_format;
    bench_scene_t scene;
    double churn_rate;
    double churn_initial_ratio;
    uint32_t worker_count;
    uint32_t workers[BENCH_SWEEP_WORKER_COUNT_MAX];
} bench_options_t;

typedef struct bench_scheduler_case_s {
    uint32_t workers;
    double spawn_ms;
    double simulate_ms;
    double simulate_entities_per_sec;
    uint64_t touched_entities;
    double checksum;
    double speedup_vs_serial;
    uint64_t structural_ops;
    lt_world_stats_t stats;
    lt_query_schedule_stats_t schedule_stats;
} bench_scheduler_case_t;

typedef struct bench_results_s {
    double spawn_ms;
    double simulate_ms;
    double simulate_entities_per_sec;
    uint64_t touched_entities;
    double checksum;
    uint32_t scheduler_case_count;
    bench_scheduler_case_t scheduler_cases[BENCH_SWEEP_WORKER_COUNT_MAX];
} bench_results_t;

typedef struct bench_motion_ctx_s {
    float dt;
} bench_motion_ctx_t;

typedef struct bench_health_ctx_s {
    float drain;
} bench_health_ctx_t;

typedef struct bench_damp_ctx_s {
    float factor;
} bench_damp_ctx_t;

typedef struct bench_churn_ctx_s {
    float blend;
    float drift;
} bench_churn_ctx_t;

static uint64_t bench_now_ns(void)
{
    struct timespec ts;

    if (timespec_get(&ts, TIME_UTC) != TIME_UTC) {
        return 0u;
    }

    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

static void bench_print_usage(const char* program)
{
    fprintf(
        stderr,
        "Usage: %s [--entities N] [--frames N] [--seed N] [--defer 0|1] "
        "[--format text|csv|json] [--scene steady|churn] [--churn-rate 0..1] "
        "[--churn-initial-ratio 0..1] [--workers N[,N...]]\n",
        program);
}

static int bench_parse_u32(const char* arg, uint32_t* out_value)
{
    char* end_ptr;
    unsigned long parsed;

    if (arg == NULL || out_value == NULL || arg[0] == '\0') {
        return 1;
    }

    parsed = strtoul(arg, &end_ptr, 10);
    if (end_ptr == arg || *end_ptr != '\0' || parsed > 0xFFFFFFFFul) {
        return 1;
    }

    *out_value = (uint32_t)parsed;
    return 0;
}

static int bench_parse_output_format(const char* arg, bench_output_format_t* out_format)
{
    if (arg == NULL || out_format == NULL) {
        return 1;
    }

    if (strcmp(arg, "text") == 0) {
        *out_format = BENCH_OUTPUT_TEXT;
        return 0;
    }
    if (strcmp(arg, "csv") == 0) {
        *out_format = BENCH_OUTPUT_CSV;
        return 0;
    }
    if (strcmp(arg, "json") == 0) {
        *out_format = BENCH_OUTPUT_JSON;
        return 0;
    }

    return 1;
}

static int bench_parse_f64(const char* arg, double* out_value)
{
    char* end_ptr;
    double parsed;

    if (arg == NULL || out_value == NULL || arg[0] == '\0') {
        return 1;
    }

    parsed = strtod(arg, &end_ptr);
    if (end_ptr == arg || *end_ptr != '\0' || parsed != parsed) {
        return 1;
    }

    *out_value = parsed;
    return 0;
}

static int bench_parse_unit_f64(const char* arg, double* out_value)
{
    double parsed;

    if (bench_parse_f64(arg, &parsed) != 0) {
        return 1;
    }
    if (parsed < 0.0 || parsed > 1.0) {
        return 1;
    }

    *out_value = parsed;
    return 0;
}

static int bench_parse_scene(const char* arg, bench_scene_t* out_scene)
{
    if (arg == NULL || out_scene == NULL) {
        return 1;
    }

    if (strcmp(arg, "steady") == 0) {
        *out_scene = BENCH_SCENE_STEADY;
        return 0;
    }
    if (strcmp(arg, "churn") == 0) {
        *out_scene = BENCH_SCENE_CHURN;
        return 0;
    }

    return 1;
}

static int bench_parse_workers(const char* arg, uint32_t* out_workers, uint32_t* out_count)
{
    const char* cursor;
    uint32_t count;

    if (arg == NULL || out_workers == NULL || out_count == NULL || arg[0] == '\0') {
        return 1;
    }

    cursor = arg;
    count = 0u;
    while (*cursor != '\0') {
        char* end_ptr;
        unsigned long parsed;
        uint32_t parsed_worker;
        uint32_t j;

        parsed = strtoul(cursor, &end_ptr, 10);
        if (end_ptr == cursor || parsed == 0ul || parsed > 0xFFFFFFFFul) {
            return 1;
        }
        if (count >= BENCH_SWEEP_WORKER_COUNT_MAX) {
            return 1;
        }

        parsed_worker = (uint32_t)parsed;
        for (j = 0u; j < count; ++j) {
            if (out_workers[j] == parsed_worker) {
                return 1;
            }
        }
        out_workers[count] = parsed_worker;
        count += 1u;

        if (*end_ptr == '\0') {
            break;
        }
        if (*end_ptr != ',') {
            return 1;
        }
        cursor = end_ptr + 1;
        if (*cursor == '\0') {
            return 1;
        }
    }

    *out_count = count;
    return count == 0u ? 1 : 0;
}

static int bench_parse_options(int argc, char** argv, bench_options_t* out_opts)
{
    int i;

    if (out_opts == NULL) {
        return 1;
    }

    memset(out_opts, 0, sizeof(*out_opts));
    out_opts->entity_count = 200000u;
    out_opts->frame_count = 120u;
    out_opts->seed = 1337u;
    out_opts->use_defer = 1u;
    out_opts->output_format = BENCH_OUTPUT_TEXT;
    out_opts->scene = BENCH_SCENE_STEADY;
    out_opts->churn_rate = 0.125;
    out_opts->churn_initial_ratio = 0.5;
    out_opts->worker_count = BENCH_SWEEP_WORKER_COUNT_DEFAULT;
    out_opts->workers[0] = 1u;
    out_opts->workers[1] = 2u;
    out_opts->workers[2] = 4u;
    out_opts->workers[3] = 8u;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--entities") == 0) {
            if (i + 1 >= argc || bench_parse_u32(argv[i + 1], &out_opts->entity_count) != 0) {
                return 1;
            }
            i += 1;
        } else if (strcmp(argv[i], "--frames") == 0) {
            if (i + 1 >= argc || bench_parse_u32(argv[i + 1], &out_opts->frame_count) != 0) {
                return 1;
            }
            i += 1;
        } else if (strcmp(argv[i], "--seed") == 0) {
            if (i + 1 >= argc || bench_parse_u32(argv[i + 1], &out_opts->seed) != 0) {
                return 1;
            }
            i += 1;
        } else if (strcmp(argv[i], "--defer") == 0) {
            uint32_t defer_value;
            if (i + 1 >= argc || bench_parse_u32(argv[i + 1], &defer_value) != 0 || defer_value > 1u) {
                return 1;
            }
            out_opts->use_defer = (uint8_t)defer_value;
            i += 1;
        } else if (strcmp(argv[i], "--format") == 0) {
            if (i + 1 >= argc
                || bench_parse_output_format(argv[i + 1], &out_opts->output_format) != 0) {
                return 1;
            }
            i += 1;
        } else if (strcmp(argv[i], "--scene") == 0) {
            if (i + 1 >= argc || bench_parse_scene(argv[i + 1], &out_opts->scene) != 0) {
                return 1;
            }
            i += 1;
        } else if (strcmp(argv[i], "--churn-rate") == 0) {
            if (i + 1 >= argc || bench_parse_unit_f64(argv[i + 1], &out_opts->churn_rate) != 0) {
                return 1;
            }
            i += 1;
        } else if (strcmp(argv[i], "--churn-initial-ratio") == 0) {
            if (i + 1 >= argc
                || bench_parse_unit_f64(argv[i + 1], &out_opts->churn_initial_ratio) != 0) {
                return 1;
            }
            i += 1;
        } else if (strcmp(argv[i], "--workers") == 0) {
            if (i + 1 >= argc
                || bench_parse_workers(
                    argv[i + 1],
                    out_opts->workers,
                    &out_opts->worker_count)
                    != 0) {
                return 1;
            }
            i += 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            return 1;
        } else {
            return 1;
        }
    }

    return 0;
}

static uint32_t bench_rand_u32(uint32_t* state)
{
    *state = (*state * 1664525u) + 1013904223u;
    return *state;
}

static float bench_rand_range(uint32_t* state, float min_value, float max_value)
{
    float t;
    uint32_t r;

    r = bench_rand_u32(state);
    t = (float)(r >> 8) / (float)0x00FFFFFFu;
    return min_value + (max_value - min_value) * t;
}

static void bench_motion_chunk(
    const lt_chunk_view_t* view,
    uint32_t worker_index,
    void* user_data)
{
    bench_motion_ctx_t* ctx;
    bench_vec3_t* position_col;
    bench_vec3_t* velocity_col;
    uint32_t row;

    (void)worker_index;

    if (view == NULL || user_data == NULL || view->columns == NULL || view->column_count < 2u) {
        return;
    }

    ctx = (bench_motion_ctx_t*)user_data;
    position_col = (bench_vec3_t*)view->columns[0];
    velocity_col = (bench_vec3_t*)view->columns[1];

    for (row = 0u; row < view->count; ++row) {
        position_col[row].x += velocity_col[row].x * ctx->dt;
        position_col[row].y += velocity_col[row].y * ctx->dt;
        position_col[row].z += velocity_col[row].z * ctx->dt;
    }
}

static void bench_health_chunk(
    const lt_chunk_view_t* view,
    uint32_t worker_index,
    void* user_data)
{
    bench_health_ctx_t* ctx;
    bench_health_t* health_col;
    uint32_t row;

    (void)worker_index;

    if (view == NULL || user_data == NULL || view->columns == NULL || view->column_count < 1u) {
        return;
    }

    ctx = (bench_health_ctx_t*)user_data;
    health_col = (bench_health_t*)view->columns[0];

    for (row = 0u; row < view->count; ++row) {
        health_col[row].value -= ctx->drain;
    }
}

static void bench_damp_chunk(
    const lt_chunk_view_t* view,
    uint32_t worker_index,
    void* user_data)
{
    bench_damp_ctx_t* ctx;
    bench_vec3_t* velocity_col;
    uint32_t row;

    (void)worker_index;

    if (view == NULL || user_data == NULL || view->columns == NULL || view->column_count < 1u) {
        return;
    }

    ctx = (bench_damp_ctx_t*)user_data;
    velocity_col = (bench_vec3_t*)view->columns[0];

    for (row = 0u; row < view->count; ++row) {
        velocity_col[row].x *= ctx->factor;
        velocity_col[row].y *= ctx->factor;
        velocity_col[row].z *= ctx->factor;
    }
}

static void bench_churn_chunk(
    const lt_chunk_view_t* view,
    uint32_t worker_index,
    void* user_data)
{
    bench_churn_ctx_t* ctx;
    bench_churn_t* churn_col;
    uint32_t row;

    (void)worker_index;

    if (view == NULL || user_data == NULL || view->columns == NULL || view->column_count < 1u) {
        return;
    }

    ctx = (bench_churn_ctx_t*)user_data;
    churn_col = (bench_churn_t*)view->columns[0];
    for (row = 0u; row < view->count; ++row) {
        churn_col[row].resistance = (churn_col[row].resistance * ctx->blend) + ctx->drift;
    }
}

static lt_status_t bench_compute_checksum(
    lt_world_t* world,
    lt_component_id_t position_id,
    lt_component_id_t velocity_id,
    lt_component_id_t health_id,
    double* out_checksum,
    uint64_t* out_entity_count)
{
    lt_query_term_t terms[3];
    lt_query_desc_t desc;
    lt_query_t* query;
    lt_query_iter_t iter;
    lt_chunk_view_t view;
    uint8_t has_value;
    double checksum;
    uint64_t entity_count;
    lt_status_t status;
    uint32_t i;

    if (world == NULL || out_checksum == NULL || out_entity_count == NULL) {
        return LT_STATUS_INVALID_ARGUMENT;
    }

    *out_checksum = 0.0;
    *out_entity_count = 0u;

    memset(terms, 0, sizeof(terms));
    terms[0].component_id = position_id;
    terms[0].access = LT_ACCESS_READ;
    terms[1].component_id = velocity_id;
    terms[1].access = LT_ACCESS_READ;
    terms[2].component_id = health_id;
    terms[2].access = LT_ACCESS_READ;

    memset(&desc, 0, sizeof(desc));
    desc.with_terms = terms;
    desc.with_count = 3u;

    status = lt_query_create(world, &desc, &query);
    if (status != LT_STATUS_OK) {
        return status;
    }

    status = lt_query_iter_begin(query, &iter);
    if (status != LT_STATUS_OK) {
        lt_query_destroy(query);
        return status;
    }

    checksum = 0.0;
    entity_count = 0u;
    while (1) {
        bench_vec3_t* position_col;
        bench_vec3_t* velocity_col;
        bench_health_t* health_col;

        status = lt_query_iter_next(&iter, &view, &has_value);
        if (status != LT_STATUS_OK) {
            lt_query_destroy(query);
            return status;
        }
        if (has_value == 0u) {
            break;
        }

        position_col = (bench_vec3_t*)view.columns[0];
        velocity_col = (bench_vec3_t*)view.columns[1];
        health_col = (bench_health_t*)view.columns[2];
        for (i = 0u; i < view.count; ++i) {
            checksum += (double)position_col[i].x;
            checksum += (double)position_col[i].y * 0.25;
            checksum += (double)position_col[i].z * 0.125;
            checksum += (double)velocity_col[i].x * 0.5;
            checksum += (double)velocity_col[i].y * 0.125;
            checksum += (double)velocity_col[i].z * 0.0625;
            checksum += (double)health_col[i].value * 0.03125;
        }

        entity_count += (uint64_t)view.count;
    }

    lt_query_destroy(query);
    *out_checksum = checksum;
    *out_entity_count = entity_count;
    return LT_STATUS_OK;
}

static int bench_run_scheduler_case(
    const bench_options_t* opts,
    uint32_t workers,
    bench_scheduler_case_t* out_case)
{
    lt_world_t* world;
    lt_component_desc_t desc;
    lt_component_id_t position_id;
    lt_component_id_t velocity_id;
    lt_component_id_t health_id;
    lt_component_id_t churn_id;
    lt_query_term_t motion_terms[2];
    lt_query_term_t health_terms[1];
    lt_query_term_t damp_terms[1];
    lt_query_term_t churn_terms[1];
    lt_query_desc_t query_desc;
    lt_query_t* motion_query;
    lt_query_t* health_query;
    lt_query_t* damp_query;
    lt_query_t* churn_query;
    lt_query_schedule_entry_t entries[4];
    bench_motion_ctx_t motion_ctx;
    bench_health_ctx_t health_ctx;
    bench_damp_ctx_t damp_ctx;
    bench_churn_ctx_t churn_ctx;
    lt_entity_t* tracked_entities;
    uint8_t* has_churn;
    uint32_t toggle_count_per_frame;
    uint64_t structural_ops;
    uint32_t schedule_entry_count;
    uint32_t random_state;
    uint64_t spawn_start_ns;
    uint64_t spawn_end_ns;
    uint64_t sim_start_ns;
    uint64_t sim_end_ns;
    uint32_t i;
    uint32_t frame;
    double sim_seconds;
    lt_status_t status;

#define BENCH_CASE_REQUIRE_STATUS(call_expr)                                                   \
    do {                                                                                        \
        status = (call_expr);                                                                   \
        if (status != LT_STATUS_OK) {                                                          \
            fprintf(stderr, "Error: %s failed with %s\\n", #call_expr, lt_status_string(status)); \
            goto cleanup;                                                                       \
        }                                                                                       \
    } while (0)

    if (opts == NULL || out_case == NULL || workers == 0u) {
        return 1;
    }

    memset(out_case, 0, sizeof(*out_case));
    out_case->workers = workers;

    world = NULL;
    motion_query = NULL;
    health_query = NULL;
    damp_query = NULL;
    churn_query = NULL;
    tracked_entities = NULL;
    has_churn = NULL;
    toggle_count_per_frame = 0u;
    structural_ops = 0u;
    schedule_entry_count = 0u;

    BENCH_CASE_REQUIRE_STATUS(lt_world_create(NULL, &world));

    memset(&desc, 0, sizeof(desc));
    desc.name = "Position";
    desc.size = (uint32_t)sizeof(bench_vec3_t);
    desc.align = (uint32_t)_Alignof(bench_vec3_t);
    BENCH_CASE_REQUIRE_STATUS(lt_register_component(world, &desc, &position_id));

    desc.name = "Velocity";
    BENCH_CASE_REQUIRE_STATUS(lt_register_component(world, &desc, &velocity_id));

    desc.name = "Health";
    desc.size = (uint32_t)sizeof(bench_health_t);
    desc.align = (uint32_t)_Alignof(bench_health_t);
    BENCH_CASE_REQUIRE_STATUS(lt_register_component(world, &desc, &health_id));

    churn_id = LT_COMPONENT_INVALID;
    if (opts->scene == BENCH_SCENE_CHURN) {
        desc.name = "Churn";
        desc.size = (uint32_t)sizeof(bench_churn_t);
        desc.align = (uint32_t)_Alignof(bench_churn_t);
        BENCH_CASE_REQUIRE_STATUS(lt_register_component(world, &desc, &churn_id));
    }

    BENCH_CASE_REQUIRE_STATUS(lt_world_reserve_entities(world, opts->entity_count));

    if (opts->scene == BENCH_SCENE_CHURN && opts->entity_count > 0u) {
        double raw_toggle_count;

        tracked_entities = (lt_entity_t*)malloc(sizeof(*tracked_entities) * (size_t)opts->entity_count);
        has_churn = (uint8_t*)malloc(sizeof(*has_churn) * (size_t)opts->entity_count);
        if (tracked_entities == NULL || has_churn == NULL) {
            fprintf(stderr, "Error: failed to allocate churn tracking buffers\n");
            goto cleanup;
        }
        memset(has_churn, 0, sizeof(*has_churn) * (size_t)opts->entity_count);

        raw_toggle_count = (double)opts->entity_count * opts->churn_rate;
        if (raw_toggle_count >= (double)opts->entity_count) {
            toggle_count_per_frame = opts->entity_count;
        } else {
            toggle_count_per_frame = (uint32_t)raw_toggle_count;
            if (opts->churn_rate > 0.0 && toggle_count_per_frame == 0u) {
                toggle_count_per_frame = 1u;
            }
        }
    }

    random_state = opts->seed;
    spawn_start_ns = bench_now_ns();

    if (opts->use_defer != 0u) {
        BENCH_CASE_REQUIRE_STATUS(lt_world_begin_defer(world));
    }

    for (i = 0u; i < opts->entity_count; ++i) {
        lt_entity_t entity;
        bench_vec3_t position;
        bench_vec3_t velocity;
        bench_health_t health;
        bench_churn_t churn;

        BENCH_CASE_REQUIRE_STATUS(lt_entity_create(world, &entity));

        position.x = bench_rand_range(&random_state, -100.0f, 100.0f);
        position.y = bench_rand_range(&random_state, -100.0f, 100.0f);
        position.z = bench_rand_range(&random_state, -100.0f, 100.0f);

        velocity.x = bench_rand_range(&random_state, -2.0f, 2.0f);
        velocity.y = bench_rand_range(&random_state, -2.0f, 2.0f);
        velocity.z = bench_rand_range(&random_state, -2.0f, 2.0f);

        health.value = bench_rand_range(&random_state, 50.0f, 150.0f);

        BENCH_CASE_REQUIRE_STATUS(lt_add_component(world, entity, position_id, &position));
        BENCH_CASE_REQUIRE_STATUS(lt_add_component(world, entity, velocity_id, &velocity));
        BENCH_CASE_REQUIRE_STATUS(lt_add_component(world, entity, health_id, &health));

        if (opts->scene == BENCH_SCENE_CHURN) {
            tracked_entities[i] = entity;
            if (opts->churn_initial_ratio >= 1.0
                || (opts->churn_initial_ratio > 0.0
                    && bench_rand_range(&random_state, 0.0f, 1.0f) < (float)opts->churn_initial_ratio)) {
                churn.resistance = bench_rand_range(&random_state, 0.1f, 2.0f);
                BENCH_CASE_REQUIRE_STATUS(lt_add_component(world, entity, churn_id, &churn));
                has_churn[i] = 1u;
            }
        }
    }

    if (opts->use_defer != 0u) {
        BENCH_CASE_REQUIRE_STATUS(lt_world_end_defer(world));
        BENCH_CASE_REQUIRE_STATUS(lt_world_flush(world));
    }

    spawn_end_ns = bench_now_ns();

    memset(motion_terms, 0, sizeof(motion_terms));
    motion_terms[0].component_id = position_id;
    motion_terms[0].access = LT_ACCESS_WRITE;
    motion_terms[1].component_id = velocity_id;
    motion_terms[1].access = LT_ACCESS_READ;

    memset(&query_desc, 0, sizeof(query_desc));
    query_desc.with_terms = motion_terms;
    query_desc.with_count = 2u;
    BENCH_CASE_REQUIRE_STATUS(lt_query_create(world, &query_desc, &motion_query));

    memset(health_terms, 0, sizeof(health_terms));
    health_terms[0].component_id = health_id;
    health_terms[0].access = LT_ACCESS_WRITE;

    memset(&query_desc, 0, sizeof(query_desc));
    query_desc.with_terms = health_terms;
    query_desc.with_count = 1u;
    BENCH_CASE_REQUIRE_STATUS(lt_query_create(world, &query_desc, &health_query));

    memset(damp_terms, 0, sizeof(damp_terms));
    damp_terms[0].component_id = velocity_id;
    damp_terms[0].access = LT_ACCESS_WRITE;

    memset(&query_desc, 0, sizeof(query_desc));
    query_desc.with_terms = damp_terms;
    query_desc.with_count = 1u;
    BENCH_CASE_REQUIRE_STATUS(lt_query_create(world, &query_desc, &damp_query));

    motion_ctx.dt = 1.0f / 60.0f;
    health_ctx.drain = 0.01f;
    damp_ctx.factor = 0.9995f;

    entries[0].query = motion_query;
    entries[0].callback = bench_motion_chunk;
    entries[0].user_data = &motion_ctx;
    entries[1].query = health_query;
    entries[1].callback = bench_health_chunk;
    entries[1].user_data = &health_ctx;
    entries[2].query = damp_query;
    entries[2].callback = bench_damp_chunk;
    entries[2].user_data = &damp_ctx;
    schedule_entry_count = 3u;

    if (opts->scene == BENCH_SCENE_CHURN) {
        memset(churn_terms, 0, sizeof(churn_terms));
        churn_terms[0].component_id = churn_id;
        churn_terms[0].access = LT_ACCESS_WRITE;

        memset(&query_desc, 0, sizeof(query_desc));
        query_desc.with_terms = churn_terms;
        query_desc.with_count = 1u;
        BENCH_CASE_REQUIRE_STATUS(lt_query_create(world, &query_desc, &churn_query));

        churn_ctx.blend = 0.996f;
        churn_ctx.drift = 0.0015f;

        entries[3].query = churn_query;
        entries[3].callback = bench_churn_chunk;
        entries[3].user_data = &churn_ctx;
        schedule_entry_count = 4u;
    }

    sim_start_ns = bench_now_ns();
    for (frame = 0u; frame < opts->frame_count; ++frame) {
        lt_query_schedule_stats_t frame_stats;

        BENCH_CASE_REQUIRE_STATUS(
            lt_query_schedule_execute(entries, schedule_entry_count, workers, &frame_stats));
        if (frame == 0u) {
            out_case->schedule_stats = frame_stats;
        }

        if (opts->scene == BENCH_SCENE_CHURN && opts->entity_count > 0u
            && toggle_count_per_frame > 0u) {
            uint32_t base;
            uint32_t op;

            base = (frame * toggle_count_per_frame) % opts->entity_count;
            if (opts->use_defer != 0u) {
                BENCH_CASE_REQUIRE_STATUS(lt_world_begin_defer(world));
            }

            for (op = 0u; op < toggle_count_per_frame; ++op) {
                uint32_t idx;

                idx = (base + op) % opts->entity_count;
                if (has_churn[idx] != 0u) {
                    BENCH_CASE_REQUIRE_STATUS(
                        lt_remove_component(world, tracked_entities[idx], churn_id));
                    has_churn[idx] = 0u;
                } else {
                    bench_churn_t churn;

                    churn.resistance = bench_rand_range(&random_state, 0.1f, 2.0f);
                    BENCH_CASE_REQUIRE_STATUS(
                        lt_add_component(world, tracked_entities[idx], churn_id, &churn));
                    has_churn[idx] = 1u;
                }
                structural_ops += 1u;
            }

            if (opts->use_defer != 0u) {
                BENCH_CASE_REQUIRE_STATUS(lt_world_end_defer(world));
                BENCH_CASE_REQUIRE_STATUS(lt_world_flush(world));
            }
        }
    }
    sim_end_ns = bench_now_ns();

    BENCH_CASE_REQUIRE_STATUS(bench_compute_checksum(
        world,
        position_id,
        velocity_id,
        health_id,
        &out_case->checksum,
        &out_case->touched_entities));
    BENCH_CASE_REQUIRE_STATUS(lt_world_get_stats(world, &out_case->stats));

    out_case->structural_ops = structural_ops;
    out_case->touched_entities = (uint64_t)out_case->stats.live_entities * (uint64_t)opts->frame_count
                                 * (opts->scene == BENCH_SCENE_CHURN ? 4u : 3u)
                                 + out_case->structural_ops;

    out_case->spawn_ms = (double)(spawn_end_ns - spawn_start_ns) / 1000000.0;
    out_case->simulate_ms = (double)(sim_end_ns - sim_start_ns) / 1000000.0;
    sim_seconds = (double)(sim_end_ns - sim_start_ns) / 1000000000.0;
    out_case->simulate_entities_per_sec = out_case->touched_entities == 0u || sim_seconds <= 0.0
                                              ? 0.0
                                              : ((double)out_case->touched_entities / sim_seconds);

    lt_query_destroy(churn_query);
    lt_query_destroy(damp_query);
    lt_query_destroy(health_query);
    lt_query_destroy(motion_query);
    lt_world_destroy(world);
    free(has_churn);
    free(tracked_entities);
#undef BENCH_CASE_REQUIRE_STATUS
    return 0;

cleanup:
    lt_query_destroy(churn_query);
    lt_query_destroy(damp_query);
    lt_query_destroy(health_query);
    lt_query_destroy(motion_query);
    lt_world_destroy(world);
    free(has_churn);
    free(tracked_entities);
#undef BENCH_CASE_REQUIRE_STATUS
    return 1;
}

static const char* bench_scene_name(bench_scene_t scene)
{
    switch (scene) {
        case BENCH_SCENE_CHURN:
            return "churn";
        case BENCH_SCENE_STEADY:
        default:
            return "steady";
    }
}

static void bench_print_results_text(
    const bench_options_t* opts,
    const bench_results_t* results,
    const lt_world_stats_t* stats)
{
    uint32_t i;

    printf("entities=%" PRIu32 "\n", opts->entity_count);
    printf("frames=%" PRIu32 "\n", opts->frame_count);
    printf("seed=%" PRIu32 "\n", opts->seed);
    printf("defer=%" PRIu32 "\n", (uint32_t)opts->use_defer);
    printf("scene=%s\n", bench_scene_name(opts->scene));
    printf("churn_rate=%.6f\n", opts->churn_rate);
    printf("churn_initial_ratio=%.6f\n", opts->churn_initial_ratio);
    printf("spawn_ms=%.3f\n", results->spawn_ms);
    printf("simulate_ms=%.3f\n", results->simulate_ms);
    printf("touched_entities=%" PRIu64 "\n", results->touched_entities);
    printf("simulate_entities_per_sec=%.3f\n", results->simulate_entities_per_sec);
    printf("checksum=%.6f\n", results->checksum);
    printf(
        "stats_live=%" PRIu32 " stats_archetypes=%" PRIu32 " stats_chunks=%" PRIu32
        " stats_pending=%" PRIu32 " stats_structural_moves=%" PRIu64 "\n",
        stats->live_entities,
        stats->archetype_count,
        stats->chunk_count,
        stats->pending_commands,
        stats->structural_moves);

    printf("scheduler_sweep_count=%" PRIu32 "\n", results->scheduler_case_count);
    for (i = 0u; i < results->scheduler_case_count; ++i) {
        const bench_scheduler_case_t* c;

        c = &results->scheduler_cases[i];
        printf(
            "scheduler_workers=%" PRIu32 " scheduler_spawn_ms=%.3f scheduler_simulate_ms=%.3f"
            " scheduler_speedup_vs_serial=%.3f scheduler_touched_entities=%" PRIu64
            " scheduler_entities_per_sec=%.3f scheduler_checksum=%.6f"
            " scheduler_structural_ops=%" PRIu64 " scheduler_batches=%" PRIu32 " scheduler_edges=%" PRIu32
            " scheduler_max_batch_size=%" PRIu32 "\n",
            c->workers,
            c->spawn_ms,
            c->simulate_ms,
            c->speedup_vs_serial,
            c->touched_entities,
            c->simulate_entities_per_sec,
            c->checksum,
            c->structural_ops,
            c->schedule_stats.batch_count,
            c->schedule_stats.edge_count,
            c->schedule_stats.max_batch_size);
    }
}

static void bench_print_results_csv(const bench_options_t* opts, const bench_results_t* results)
{
    uint32_t i;

    printf(
        "entities,frames,seed,defer,workers,spawn_ms,simulate_ms,speedup_vs_serial,"
        "touched_entities,simulate_entities_per_sec,checksum,stats_live,stats_archetypes,"
        "stats_chunks,stats_pending,stats_structural_moves,schedule_batch_count,"
        "schedule_edge_count,schedule_max_batch_size,scheduler_structural_ops,scene,"
        "churn_rate,churn_initial_ratio\n");

    for (i = 0u; i < results->scheduler_case_count; ++i) {
        const bench_scheduler_case_t* c;

        c = &results->scheduler_cases[i];
        printf(
            "%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ","
            "%.3f,%.3f,%.3f,%" PRIu64 ",%.3f,%.6f,%" PRIu32 ",%" PRIu32 ",%" PRIu32
            ",%" PRIu32 ",%" PRIu64 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu64 ",%s,%.6f,%.6f\n",
            opts->entity_count,
            opts->frame_count,
            opts->seed,
            (uint32_t)opts->use_defer,
            c->workers,
            c->spawn_ms,
            c->simulate_ms,
            c->speedup_vs_serial,
            c->touched_entities,
            c->simulate_entities_per_sec,
            c->checksum,
            c->stats.live_entities,
            c->stats.archetype_count,
            c->stats.chunk_count,
            c->stats.pending_commands,
            c->stats.structural_moves,
            c->schedule_stats.batch_count,
            c->schedule_stats.edge_count,
            c->schedule_stats.max_batch_size,
            c->structural_ops,
            bench_scene_name(opts->scene),
            opts->churn_rate,
            opts->churn_initial_ratio);
    }
}

static void bench_print_results_json(
    const bench_options_t* opts,
    const bench_results_t* results,
    const lt_world_stats_t* stats)
{
    uint32_t i;

    printf("{\n");
    printf("  \"entities\": %" PRIu32 ",\n", opts->entity_count);
    printf("  \"frames\": %" PRIu32 ",\n", opts->frame_count);
    printf("  \"seed\": %" PRIu32 ",\n", opts->seed);
    printf("  \"defer\": %s,\n", opts->use_defer != 0u ? "true" : "false");
    printf("  \"scene\": \"%s\",\n", bench_scene_name(opts->scene));
    printf("  \"churn_rate\": %.6f,\n", opts->churn_rate);
    printf("  \"churn_initial_ratio\": %.6f,\n", opts->churn_initial_ratio);
    printf("  \"spawn_ms\": %.3f,\n", results->spawn_ms);
    printf("  \"simulate_ms\": %.3f,\n", results->simulate_ms);
    printf("  \"touched_entities\": %" PRIu64 ",\n", results->touched_entities);
    printf("  \"simulate_entities_per_sec\": %.3f,\n", results->simulate_entities_per_sec);
    printf("  \"checksum\": %.6f,\n", results->checksum);
    printf("  \"stats_live\": %" PRIu32 ",\n", stats->live_entities);
    printf("  \"stats_archetypes\": %" PRIu32 ",\n", stats->archetype_count);
    printf("  \"stats_chunks\": %" PRIu32 ",\n", stats->chunk_count);
    printf("  \"stats_pending\": %" PRIu32 ",\n", stats->pending_commands);
    printf("  \"stats_structural_moves\": %" PRIu64 ",\n", stats->structural_moves);
    printf("  \"scheduler_sweep\": [\n");

    for (i = 0u; i < results->scheduler_case_count; ++i) {
        const bench_scheduler_case_t* c;

        c = &results->scheduler_cases[i];
        printf("    {\n");
        printf("      \"workers\": %" PRIu32 ",\n", c->workers);
        printf("      \"spawn_ms\": %.3f,\n", c->spawn_ms);
        printf("      \"simulate_ms\": %.3f,\n", c->simulate_ms);
        printf("      \"speedup_vs_serial\": %.3f,\n", c->speedup_vs_serial);
        printf("      \"touched_entities\": %" PRIu64 ",\n", c->touched_entities);
        printf("      \"simulate_entities_per_sec\": %.3f,\n", c->simulate_entities_per_sec);
        printf("      \"checksum\": %.6f,\n", c->checksum);
        printf("      \"structural_ops\": %" PRIu64 ",\n", c->structural_ops);
        printf("      \"stats_live\": %" PRIu32 ",\n", c->stats.live_entities);
        printf("      \"stats_archetypes\": %" PRIu32 ",\n", c->stats.archetype_count);
        printf("      \"stats_chunks\": %" PRIu32 ",\n", c->stats.chunk_count);
        printf("      \"stats_pending\": %" PRIu32 ",\n", c->stats.pending_commands);
        printf("      \"stats_structural_moves\": %" PRIu64 ",\n", c->stats.structural_moves);
        printf("      \"schedule_batch_count\": %" PRIu32 ",\n", c->schedule_stats.batch_count);
        printf("      \"schedule_edge_count\": %" PRIu32 ",\n", c->schedule_stats.edge_count);
        printf(
            "      \"schedule_max_batch_size\": %" PRIu32 "\n",
            c->schedule_stats.max_batch_size);
        printf("    }%s\n", (i + 1u) < results->scheduler_case_count ? "," : "");
    }

    printf("  ]\n");
    printf("}\n");
}

static void bench_print_results(
    const bench_options_t* opts,
    const bench_results_t* results,
    const lt_world_stats_t* stats)
{
    switch (opts->output_format) {
        case BENCH_OUTPUT_CSV:
            bench_print_results_csv(opts, results);
            break;
        case BENCH_OUTPUT_JSON:
            bench_print_results_json(opts, results, stats);
            break;
        case BENCH_OUTPUT_TEXT:
        default:
            bench_print_results_text(opts, results, stats);
            break;
    }
}

int main(int argc, char** argv)
{
    bench_options_t opts;
    bench_results_t results;
    lt_world_stats_t baseline_stats;
    double serial_simulate_ms;
    uint32_t i;

    if (bench_parse_options(argc, argv, &opts) != 0) {
        bench_print_usage(argv[0]);
        return 1;
    }

    memset(&results, 0, sizeof(results));
    results.scheduler_case_count = opts.worker_count;

    for (i = 0u; i < opts.worker_count; ++i) {
        if (bench_run_scheduler_case(&opts, opts.workers[i], &results.scheduler_cases[i]) != 0) {
            return 1;
        }
    }

    serial_simulate_ms = results.scheduler_cases[0].simulate_ms;
    for (i = 0u; i < results.scheduler_case_count; ++i) {
        if (serial_simulate_ms <= 0.0 || results.scheduler_cases[i].simulate_ms <= 0.0) {
            results.scheduler_cases[i].speedup_vs_serial = 0.0;
        } else {
            results.scheduler_cases[i].speedup_vs_serial =
                serial_simulate_ms / results.scheduler_cases[i].simulate_ms;
        }
    }

    results.spawn_ms = results.scheduler_cases[0].spawn_ms;
    results.simulate_ms = results.scheduler_cases[0].simulate_ms;
    results.touched_entities = results.scheduler_cases[0].touched_entities;
    results.simulate_entities_per_sec = results.scheduler_cases[0].simulate_entities_per_sec;
    results.checksum = results.scheduler_cases[0].checksum;
    baseline_stats = results.scheduler_cases[0].stats;

    bench_print_results(&opts, &results, &baseline_stats);
    return 0;
}
