if(NOT DEFINED BENCH_EXE)
    message(FATAL_ERROR "BENCH_EXE is required")
endif()

if(NOT DEFINED MODE)
    message(FATAL_ERROR "MODE must be one of: text, csv, json")
endif()

execute_process(
    COMMAND "${BENCH_EXE}" --entities 10000 --frames 5 --seed 7 --format "${MODE}"
    RESULT_VARIABLE bench_status
    OUTPUT_VARIABLE bench_output
    ERROR_VARIABLE bench_error
)

if(NOT bench_status EQUAL 0)
    message(FATAL_ERROR
        "lattice_bench failed for mode=${MODE} with exit=${bench_status}\n"
        "stderr:\n${bench_error}\nstdout:\n${bench_output}")
endif()

function(assert_output_contains needle)
    string(FIND "${bench_output}" "${needle}" needle_pos)
    if(needle_pos EQUAL -1)
        message(FATAL_ERROR
            "Missing expected output fragment: ${needle}\n"
            "mode=${MODE}\nstdout:\n${bench_output}\nstderr:\n${bench_error}")
    endif()
endfunction()

if(MODE STREQUAL "text")
    assert_output_contains("entities=10000")
    assert_output_contains("scheduler_sweep_count=4")
    assert_output_contains("scheduler_workers=1")
    assert_output_contains("scheduler_workers=2")
    assert_output_contains("scheduler_workers=4")
    assert_output_contains("scheduler_workers=8")
    assert_output_contains("scheduler_speedup_vs_serial=")
    assert_output_contains("scheduler_batches=")
elseif(MODE STREQUAL "csv")
    assert_output_contains(
        "entities,frames,seed,defer,workers,spawn_ms,simulate_ms,speedup_vs_serial,")
    assert_output_contains("schedule_batch_count,schedule_edge_count,schedule_max_batch_size")
    assert_output_contains(",7,1,1,")
    assert_output_contains(",7,1,8,")
elseif(MODE STREQUAL "json")
    assert_output_contains("\"scheduler_sweep\": [")
    assert_output_contains("\"workers\": 1")
    assert_output_contains("\"workers\": 8")
    assert_output_contains("\"speedup_vs_serial\":")
    assert_output_contains("\"schedule_batch_count\":")
else()
    message(FATAL_ERROR "Unsupported MODE=${MODE}; expected text/csv/json")
endif()
