# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

cmake_minimum_required(VERSION 3.21)

# The version lives in the public header and nowhere else: read out of it
# here rather than written a second time, so that a release is one edit and the
# library cannot report a number its package does not carry.
foreach(part MAJOR MINOR PATCH)
    file(STRINGS include/rmlines/rmlines.h line
         REGEX "^#define RMLINES_VERSION_${part} [0-9]+$")
    if(NOT line MATCHES "^#define RMLINES_VERSION_${part} ([0-9]+)$")
        message(FATAL_ERROR "RMLINES_VERSION_${part} not found in include/rmlines/rmlines.h")
    endif()
    set(RMLINES_VERSION_${part} "${CMAKE_MATCH_1}")
endforeach()

project(rmlines
    VERSION ${RMLINES_VERSION_MAJOR}.${RMLINES_VERSION_MINOR}.${RMLINES_VERSION_PATCH}
    DESCRIPTION "Reading and writing the reMarkable .rm v6 page format"
    HOMEPAGE_URL "https://github.com/altessa-s/rmlines-lib"
    LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(RMLINES_BUILD_TESTS "Build the test suite" ${PROJECT_IS_TOP_LEVEL})
option(RMLINES_BUILD_TOOLS "Build the rmdump command-line tool" ${PROJECT_IS_TOP_LEVEL})
option(RMLINES_WERROR "Turn compiler warnings into errors" OFF)
option(RMLINES_BUILD_FUZZERS "Build the libFuzzer targets (clang only)" OFF)

# ---------------------------------------------------------------------------
# The library
# ---------------------------------------------------------------------------

set(RMLINES_SOURCES
    src/rm_support.c
    src/rm_bytes.c
    src/rm_log.c
    src/rm_model.c
    src/rm_parser.c
    src/rm_writer.c
    src/rm_editor.c)

set(RMLINES_PUBLIC_HEADERS
    include/rmlines/rmlines.h
    include/rmlines/export.h
    include/rmlines/types.h
    include/rmlines/log.h
    include/rmlines/parser.h
    include/rmlines/writer.h
    include/rmlines/editor.h)

# Honours BUILD_SHARED_LIBS: static by default, shared with
# -DBUILD_SHARED_LIBS=ON. One target either way, so the exported CMake package
# and the pkg-config file describe whatever was built.
add_library(rmlines ${RMLINES_SOURCES})
add_library(RMLines::rmlines ALIAS rmlines)

target_include_directories(rmlines
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

set_target_properties(rmlines PROPERTIES
    C_STANDARD 23
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    # The plumbing — the arena, the hash map, the byte reader — is not part of
    # the contract and must not reach a consumer's symbol table.
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    PUBLIC_HEADER "${RMLINES_PUBLIC_HEADERS}")

if(BUILD_SHARED_LIBS)
    target_compile_definitions(rmlines PRIVATE RMLINES_BUILDING_SHARED)
endif()

# The warning set, and it is one set: the library is built with clang or
# gcc on every platform, Windows included. MSVC's C compiler stops at the
# first `constexpr`, `nullptr`, `[[nodiscard]]` and typed enum — all of them
# C23, all of them throughout — and a build that carries a second warning
# dialect for a compiler that cannot read the code is a build with a branch
# nobody can exercise.
target_compile_options(rmlines PRIVATE
    -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion
    -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith -Wcast-qual)
if(RMLINES_WERROR)
    target_compile_options(rmlines PRIVATE -Werror)
endif()
# `isfinite` lives in libm on the platforms that still split it out.
include(CheckLibraryExists)
check_library_exists(m ceil "" RMLINES_HAVE_LIBM)
if(RMLINES_HAVE_LIBM)
    target_link_libraries(rmlines PUBLIC m)
endif()

# ---------------------------------------------------------------------------
# The tool
# ---------------------------------------------------------------------------

if(RMLINES_BUILD_TOOLS)
    add_executable(rmdump tools/rmdump.c)
    target_link_libraries(rmdump PRIVATE rmlines)
    set_target_properties(rmdump PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON
                                            C_EXTENSIONS OFF)
endif()

# ---------------------------------------------------------------------------
# The tests
# ---------------------------------------------------------------------------

if(RMLINES_BUILD_TESTS)
    enable_testing()

    # The tests reach past the public API — a count that wraps and a UTF-8
    # repair are read where the arithmetic is, not through a whole page — so
    # they compile the sources again rather than link the installed contract.
    add_library(rmlines_testing STATIC EXCLUDE_FROM_ALL ${RMLINES_SOURCES})
    target_compile_definitions(rmlines_testing PUBLIC RMLINES_TESTING)
    target_include_directories(rmlines_testing PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    set_target_properties(rmlines_testing PROPERTIES
        C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(RMLINES_HAVE_LIBM)
        target_link_libraries(rmlines_testing PUBLIC m)
    endif()

    foreach(suite corpus model reader recovery writer log editor format_edges)
        add_executable(test_${suite} EXCLUDE_FROM_ALL tests/test_${suite}.c)
        target_link_libraries(test_${suite} PRIVATE rmlines_testing)
        target_include_directories(test_${suite} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        target_compile_definitions(test_${suite} PRIVATE
            RM_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
        set_target_properties(test_${suite} PROPERTIES
            C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
        add_test(NAME ${suite} COMMAND test_${suite})
    endforeach()

    # A cyclic scene must fail the test instead of hanging the CI worker.
    set_tests_properties(recovery PROPERTIES TIMEOUT 30)

    # The parser against bytes nobody wrote. Off by default and clang-only:
    # libFuzzer is a clang runtime, and the sanitizers it wants are the CI
    # job's to ask for. The sample fixtures are its seed corpus.
    if(RMLINES_BUILD_FUZZERS)
        add_executable(fuzz_parse tests/fuzz_parse.c)
        target_link_libraries(fuzz_parse PRIVATE rmlines_testing)
        target_compile_options(fuzz_parse PRIVATE -fsanitize=fuzzer)
        target_link_options(fuzz_parse PRIVATE -fsanitize=fuzzer)
        set_target_properties(fuzz_parse PROPERTIES
            C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    endif()

    # `ctest` builds nothing on its own, and a suite that was never compiled is
    # a suite that silently did not run.
    add_custom_target(build_tests DEPENDS
        test_corpus test_model test_reader test_recovery test_writer test_log test_editor
        test_format_edges)
    # Under a multi-configuration generator — Xcode, Visual Studio — a
    # `ctest` that is not told which configuration to run finds no tests at
    # all and says so five times, so the configuration is passed along. Passed
    # conditionally: under a single-configuration generator with no build type
    # set, `$<CONFIG>` is empty, and `-C ""` is an error of its own. The
    # expression yields the two arguments or nothing, and COMMAND_EXPAND_LISTS
    # is what lets "nothing" vanish rather than arrive as an empty argument.
    add_custom_target(check
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
                "$<$<BOOL:$<CONFIG>>:-C;$<CONFIG>>"
        DEPENDS build_tests
        COMMAND_EXPAND_LISTS)
endif()

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------

install(TARGETS rmlines
    EXPORT RMLinesTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rmlines)

if(RMLINES_BUILD_TOOLS)
    install(TARGETS rmdump RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(EXPORT RMLinesTargets
    FILE RMLinesTargets.cmake
    NAMESPACE RMLines::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RMLines)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RMLinesConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/RMLinesConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RMLines)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/RMLinesConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/RMLinesConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/RMLinesConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RMLines)

# pkg-config, for the builds that never learned CMake.
#
# The way back from the `.pc` to the prefix is worked out here rather than
# written as a fixed `../..`: CMAKE_INSTALL_LIBDIR is `lib` on one
# distribution, `lib64` on another and `lib/x86_64-linux-gnu` on a third, and
# only one of those is two levels down.
file(RELATIVE_PATH RMLINES_PC_PREFIX_RELATIVE
     "${CMAKE_INSTALL_FULL_LIBDIR}/pkgconfig" "${CMAKE_INSTALL_PREFIX}")
if(RMLINES_PC_PREFIX_RELATIVE STREQUAL "")
    set(RMLINES_PC_PREFIX_RELATIVE ".")
endif()
string(REGEX REPLACE "/$" "" RMLINES_PC_PREFIX_RELATIVE "${RMLINES_PC_PREFIX_RELATIVE}")

set(RMLINES_PC_LIBS_PRIVATE "")
if(RMLINES_HAVE_LIBM)
    set(RMLINES_PC_LIBS_PRIVATE "-lm")
endif()
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/rmlines.pc.in
               ${CMAKE_CURRENT_BINARY_DIR}/rmlines.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rmlines.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
