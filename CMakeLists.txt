cmake_minimum_required(VERSION 3.20)
project(lattice VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)
find_package(Threads REQUIRED)

option(LATTICE_BUILD_TESTS "Build lattice tests" ON)
option(LATTICE_BUILD_BENCHMARKS "Build lattice benchmark app" ON)

add_library(lattice
    src/world.c
)
add_library(lattice::lattice ALIAS lattice)

target_compile_features(lattice PUBLIC c_std_11)
target_include_directories(lattice
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(lattice PRIVATE Threads::Threads)

if(CMAKE_USE_PTHREADS_INIT)
    target_compile_definitions(lattice PRIVATE LT_HAS_PTHREADS=1)
endif()

if(MSVC)
    target_compile_options(lattice PRIVATE /W4 /WX)
else()
    target_compile_options(lattice PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

install(TARGETS lattice
    EXPORT latticeTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(
    EXPORT latticeTargets
    FILE latticeTargets.cmake
    NAMESPACE lattice::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lattice
)

if(LATTICE_BUILD_TESTS)
    enable_testing()
    add_executable(lattice_tests tests/test_main.c)
    target_link_libraries(lattice_tests PRIVATE lattice)
    add_test(NAME lattice_tests COMMAND lattice_tests)
endif()

if(LATTICE_BUILD_BENCHMARKS)
    add_executable(lattice_bench apps/bench/main.c)
    target_link_libraries(lattice_bench PRIVATE lattice)
    if(LATTICE_BUILD_TESTS)
        add_test(
            NAME lattice_bench_smoke
            COMMAND ${CMAKE_COMMAND}
                -DBENCH_EXE=$<TARGET_FILE:lattice_bench>
                -DMODE=text
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_bench_output.cmake
        )
        add_test(
            NAME lattice_bench_smoke_csv
            COMMAND ${CMAKE_COMMAND}
                -DBENCH_EXE=$<TARGET_FILE:lattice_bench>
                -DMODE=csv
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_bench_output.cmake
        )
        add_test(
            NAME lattice_bench_smoke_json
            COMMAND ${CMAKE_COMMAND}
                -DBENCH_EXE=$<TARGET_FILE:lattice_bench>
                -DMODE=json
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_bench_output.cmake
        )
        add_test(
            NAME lattice_bench_smoke_custom_workers
            COMMAND ${CMAKE_COMMAND}
                -DBENCH_EXE=$<TARGET_FILE:lattice_bench>
                -DMODE=text
                -DWORKERS=1,3,6
                -DEXPECTED_WORKERS=1,3,6
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_bench_output.cmake
        )
    endif()
endif()
