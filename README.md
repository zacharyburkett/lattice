# Lattice

Lattice is a data-oriented ECS framework in C with archetype/chunk storage.

> Stability notice
> This repository is pre-1.0 and not stable. APIs, scheduling behavior, and internal data layout details may change between commits.

## What Is Implemented

Core library target:

- `lattice::lattice`

Current feature set:

- World lifecycle and stats APIs
- Stable entity handles (index + generation)
- Component registration and metadata validation
- Archetype/chunk storage with structural moves
- Direct component add/remove/get/has APIs
- Query API with chunk iteration
- Deferred structural command buffer
- Experimental parallel query iteration helper
- Experimental conflict-aware query scheduler and compiled schedules
- Benchmark executable with text/csv/json output modes

## Build

```sh
cmake -S . -B build
cmake --build build
```

Run tests:

```sh
ctest --test-dir build --output-on-failure
```

Run benchmark app (when enabled):

```sh
./build/lattice_bench
```

## CMake Options

- `LATTICE_BUILD_TESTS=ON|OFF`
- `LATTICE_BUILD_BENCHMARKS=ON|OFF`

## Consumer Integration

From source:

```cmake
add_subdirectory(/absolute/path/to/lattice ${CMAKE_BINARY_DIR}/_deps/lattice EXCLUDE_FROM_ALL)
target_link_libraries(my_target PRIVATE lattice::lattice)
```

Installed package export is also generated (`latticeTargets.cmake`).

Public umbrella include:

- `include/lattice/lattice.h`

## Docs

- `docs/PROPOSAL.md`
- `docs/ARCHITECTURE.md`
- `docs/API_SKETCH.md`
- `docs/PROJECT_PLAN.md`
