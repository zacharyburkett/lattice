if(NOT DEFINED BENCH_EXE)
    message(FATAL_ERROR "BENCH_EXE is required")
endif()

if(NOT DEFINED MODE)
    message(FATAL_ERROR "MODE must be one of: text, csv, json")
endif()

if(NOT DEFINED EXPECTED_WORKERS)
    set(EXPECTED_WORKERS "1,2,4,8")
endif()

if(NOT DEFINED SCENE)
    set(SCENE "steady")
endif()

string(REPLACE "," ";" expected_worker_list "${EXPECTED_WORKERS}")
list(LENGTH expected_worker_list expected_worker_count)
if(expected_worker_count EQUAL 0)
    message(FATAL_ERROR "EXPECTED_WORKERS must contain at least one worker value")
endif()

set(bench_cmd
    "${BENCH_EXE}"
    "--entities" "10000"
    "--frames" "5"
    "--seed" "7"
    "--format" "${MODE}"
    "--scene" "${SCENE}")

if(DEFINED WORKERS)
    list(APPEND bench_cmd "--workers" "${WORKERS}")
endif()

execute_process(
    COMMAND ${bench_cmd}
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
    assert_output_contains("scene=${SCENE}")
    assert_output_contains("scheduler_sweep_count=${expected_worker_count}")
    foreach(worker ${expected_worker_list})
        assert_output_contains("scheduler_workers=${worker}")
    endforeach()
    assert_output_contains("scheduler_speedup_vs_serial=")
    assert_output_contains("scheduler_structural_ops=")
    assert_output_contains("scheduler_batches=")
elseif(MODE STREQUAL "csv")
    assert_output_contains(
        "entities,frames,seed,defer,workers,spawn_ms,simulate_ms,speedup_vs_serial,")
    assert_output_contains(
        "schedule_batch_count,schedule_edge_count,schedule_max_batch_size,scheduler_structural_ops,scene")
    foreach(worker ${expected_worker_list})
        assert_output_contains(",7,1,${worker},")
    endforeach()
    assert_output_contains(",${SCENE}")
elseif(MODE STREQUAL "json")
    assert_output_contains("\"scene\": \"${SCENE}\"")
    assert_output_contains("\"scheduler_sweep\": [")
    foreach(worker ${expected_worker_list})
        assert_output_contains("\"workers\": ${worker}")
    endforeach()
    assert_output_contains("\"speedup_vs_serial\":")
    assert_output_contains("\"structural_ops\":")
    assert_output_contains("\"schedule_batch_count\":")
else()
    message(FATAL_ERROR "Unsupported MODE=${MODE}; expected text/csv/json")
endif()
