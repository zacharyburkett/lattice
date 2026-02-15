cmake_minimum_required(VERSION 3.20)
project(lattice VERSION 0.1.0 LANGUAGES C)

include(GNUInstallDirs)

option(LATTICE_BUILD_TESTS "Build lattice tests" ON)

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
