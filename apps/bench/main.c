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

typedef enum bench_output_format_e {
    BENCH_OUTPUT_TEXT = 0,
    BENCH_OUTPUT_CSV = 1,
    BENCH_OUTPUT_JSON = 2
} bench_output_format_t;

typedef struct bench_options_s {
    uint32_t entity_count;
    uint32_t frame_count;
    uint32_t seed;
    uint8_t use_defer;
    bench_output_format_t output_format;
} bench_options_t;

typedef struct bench_results_s {
    double spawn_ms;
    double simulate_ms;
    double simulate_entities_per_sec;
    uint64_t touched_entities;
    double checksum;
} bench_results_t;

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
        "Usage: %s [--entities N] [--frames N] [--seed N] [--defer 0|1] [--format text|csv|json]\n",
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

static void bench_print_results_text(
    const bench_options_t* opts,
    const bench_results_t* results,
    const lt_world_stats_t* stats)
{
    printf("entities=%" PRIu32 "\n", opts->entity_count);
    printf("frames=%" PRIu32 "\n", opts->frame_count);
    printf("seed=%" PRIu32 "\n", opts->seed);
    printf("defer=%" PRIu32 "\n", (uint32_t)opts->use_defer);
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
}

static void bench_print_results_csv(
    const bench_options_t* opts,
    const bench_results_t* results,
    const lt_world_stats_t* stats)
{
    printf(
        "entities,frames,seed,defer,spawn_ms,simulate_ms,touched_entities,simulate_entities_per_sec,checksum,"
        "stats_live,stats_archetypes,stats_chunks,stats_pending,stats_structural_moves\n");
    printf(
        "%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%.3f,%.3f,%" PRIu64 ",%.3f,%.6f,%" PRIu32
        ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu64 "\n",
        opts->entity_count,
        opts->frame_count,
        opts->seed,
        (uint32_t)opts->use_defer,
        results->spawn_ms,
        results->simulate_ms,
        results->touched_entities,
        results->simulate_entities_per_sec,
        results->checksum,
        stats->live_entities,
        stats->archetype_count,
        stats->chunk_count,
        stats->pending_commands,
        stats->structural_moves);
}

static void bench_print_results_json(
    const bench_options_t* opts,
    const bench_results_t* results,
    const lt_world_stats_t* stats)
{
    printf("{\n");
    printf("  \"entities\": %" PRIu32 ",\n", opts->entity_count);
    printf("  \"frames\": %" PRIu32 ",\n", opts->frame_count);
    printf("  \"seed\": %" PRIu32 ",\n", opts->seed);
    printf("  \"defer\": %s,\n", opts->use_defer != 0u ? "true" : "false");
    printf("  \"spawn_ms\": %.3f,\n", results->spawn_ms);
    printf("  \"simulate_ms\": %.3f,\n", results->simulate_ms);
    printf("  \"touched_entities\": %" PRIu64 ",\n", results->touched_entities);
    printf("  \"simulate_entities_per_sec\": %.3f,\n", results->simulate_entities_per_sec);
    printf("  \"checksum\": %.6f,\n", results->checksum);
    printf("  \"stats_live\": %" PRIu32 ",\n", stats->live_entities);
    printf("  \"stats_archetypes\": %" PRIu32 ",\n", stats->archetype_count);
    printf("  \"stats_chunks\": %" PRIu32 ",\n", stats->chunk_count);
    printf("  \"stats_pending\": %" PRIu32 ",\n", stats->pending_commands);
    printf("  \"stats_structural_moves\": %" PRIu64 "\n", stats->structural_moves);
    printf("}\n");
}

static void bench_print_results(
    const bench_options_t* opts,
    const bench_results_t* results,
    const lt_world_stats_t* stats)
{
    switch (opts->output_format) {
        case BENCH_OUTPUT_CSV:
            bench_print_results_csv(opts, results, stats);
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

#define BENCH_REQUIRE_STATUS(call_expr)                                                        \
    do {                                                                                        \
        lt_status_t bench_status_result = (call_expr);                                          \
        if (bench_status_result != LT_STATUS_OK) {                                              \
            fprintf(stderr, "Error: %s failed with %s\n", #call_expr,                         \
                    lt_status_string(bench_status_result));                                     \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)

int main(int argc, char** argv)
{
    bench_options_t opts;
    lt_world_t* world;
    lt_component_desc_t desc;
    lt_component_id_t position_id;
    lt_component_id_t velocity_id;
    lt_query_term_t terms[2];
    lt_query_desc_t query_desc;
    lt_query_t* query;
    lt_world_stats_t stats;
    uint32_t random_state;
    uint64_t spawn_start_ns;
    uint64_t spawn_end_ns;
    uint64_t sim_start_ns;
    uint64_t sim_end_ns;
    uint32_t i;
    uint32_t frame;
    uint64_t touched_entities;
    double checksum;
    double sim_seconds;
    bench_results_t results;

    if (bench_parse_options(argc, argv, &opts) != 0) {
        bench_print_usage(argv[0]);
        return 1;
    }

    BENCH_REQUIRE_STATUS(lt_world_create(NULL, &world));

    memset(&desc, 0, sizeof(desc));
    desc.name = "Position";
    desc.size = (uint32_t)sizeof(bench_vec3_t);
    desc.align = (uint32_t)_Alignof(bench_vec3_t);
    BENCH_REQUIRE_STATUS(lt_register_component(world, &desc, &position_id));

    desc.name = "Velocity";
    BENCH_REQUIRE_STATUS(lt_register_component(world, &desc, &velocity_id));

    BENCH_REQUIRE_STATUS(lt_world_reserve_entities(world, opts.entity_count));

    random_state = opts.seed;
    spawn_start_ns = bench_now_ns();

    if (opts.use_defer != 0u) {
        BENCH_REQUIRE_STATUS(lt_world_begin_defer(world));
    }

    for (i = 0u; i < opts.entity_count; ++i) {
        lt_entity_t entity;
        bench_vec3_t position;
        bench_vec3_t velocity;

        BENCH_REQUIRE_STATUS(lt_entity_create(world, &entity));

        position.x = bench_rand_range(&random_state, -100.0f, 100.0f);
        position.y = bench_rand_range(&random_state, -100.0f, 100.0f);
        position.z = bench_rand_range(&random_state, -100.0f, 100.0f);

        velocity.x = bench_rand_range(&random_state, -2.0f, 2.0f);
        velocity.y = bench_rand_range(&random_state, -2.0f, 2.0f);
        velocity.z = bench_rand_range(&random_state, -2.0f, 2.0f);

        BENCH_REQUIRE_STATUS(lt_add_component(world, entity, position_id, &position));
        BENCH_REQUIRE_STATUS(lt_add_component(world, entity, velocity_id, &velocity));
    }

    if (opts.use_defer != 0u) {
        BENCH_REQUIRE_STATUS(lt_world_end_defer(world));
        BENCH_REQUIRE_STATUS(lt_world_flush(world));
    }

    spawn_end_ns = bench_now_ns();

    memset(terms, 0, sizeof(terms));
    terms[0].component_id = position_id;
    terms[0].access = LT_ACCESS_WRITE;
    terms[1].component_id = velocity_id;
    terms[1].access = LT_ACCESS_READ;

    memset(&query_desc, 0, sizeof(query_desc));
    query_desc.with_terms = terms;
    query_desc.with_count = 2u;

    BENCH_REQUIRE_STATUS(lt_query_create(world, &query_desc, &query));

    sim_start_ns = bench_now_ns();
    touched_entities = 0u;
    checksum = 0.0;

    for (frame = 0u; frame < opts.frame_count; ++frame) {
        lt_query_iter_t iter;
        lt_chunk_view_t view;
        uint8_t has_value;

        BENCH_REQUIRE_STATUS(lt_query_iter_begin(query, &iter));

        while (1) {
            uint32_t row;
            bench_vec3_t* position_col;
            bench_vec3_t* velocity_col;

            BENCH_REQUIRE_STATUS(lt_query_iter_next(&iter, &view, &has_value));
            if (has_value == 0u) {
                break;
            }

            position_col = (bench_vec3_t*)view.columns[0];
            velocity_col = (bench_vec3_t*)view.columns[1];

            for (row = 0u; row < view.count; ++row) {
                position_col[row].x += velocity_col[row].x * (1.0f / 60.0f);
                position_col[row].y += velocity_col[row].y * (1.0f / 60.0f);
                position_col[row].z += velocity_col[row].z * (1.0f / 60.0f);
                checksum += (double)position_col[row].x;
            }

            touched_entities += (uint64_t)view.count;
        }
    }

    sim_end_ns = bench_now_ns();
    sim_seconds = (double)(sim_end_ns - sim_start_ns) / 1000000000.0;

    BENCH_REQUIRE_STATUS(lt_world_get_stats(world, &stats));

    results.spawn_ms = (double)(spawn_end_ns - spawn_start_ns) / 1000000.0;
    results.simulate_ms = (double)(sim_end_ns - sim_start_ns) / 1000000.0;
    results.touched_entities = touched_entities;
    results.simulate_entities_per_sec = touched_entities == 0u || sim_seconds <= 0.0
                                            ? 0.0
                                            : ((double)touched_entities / sim_seconds);
    results.checksum = checksum;

    bench_print_results(&opts, &results, &stats);

    lt_query_destroy(query);
    lt_world_destroy(world);
    return 0;
}
