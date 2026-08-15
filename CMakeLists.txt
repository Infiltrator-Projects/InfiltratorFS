# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(InfiltratorFS VERSION 0.5.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

function(infilfs_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(infilfs_core STATIC
    src/checksum.c
    src/storage.c
    src/utf8.c
    src/fs.c
    src/volume.c
)
target_include_directories(infilfs_core PUBLIC include)
infilfs_enable_warnings(infilfs_core)

add_executable(infilfs-checksum-test tests/checksum-test.c)
target_link_libraries(infilfs-checksum-test PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-checksum-test)

add_executable(infilfs-portable-core tests/portable-core.c)
target_link_libraries(infilfs-portable-core PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-portable-core)

enable_testing()
add_test(NAME infilfs-checksum-known-answer COMMAND infilfs-checksum-test)

# Linux/POSIX is the first adapter and test host. The core library and disk
# format do not depend on this adapter.
if(UNIX)
    add_library(infilfs_posix STATIC src/platform/posix_io.c)
    target_include_directories(infilfs_posix PUBLIC include)
    target_link_libraries(infilfs_posix PUBLIC infilfs_core)
    target_compile_definitions(infilfs_posix PRIVATE _GNU_SOURCE)
    infilfs_enable_warnings(infilfs_posix)

    add_executable(mkfs.infilfs tools/mkfs.infilfs.c)
    add_executable(infilfs-inspect tools/infilfs-inspect.c)
    add_executable(infilfs-tool tools/infilfs-tool.c)
    add_executable(infilfs-scrub tools/infilfs-scrub.c)
    foreach(tool_target IN ITEMS mkfs.infilfs infilfs-inspect infilfs-tool infilfs-scrub)
        target_link_libraries(${tool_target} PRIVATE infilfs_posix)
        target_compile_definitions(${tool_target} PRIVATE _GNU_SOURCE)
        infilfs_enable_warnings(${tool_target})
    endforeach()

    add_executable(infilfs-phase1-api tests/phase1-api.c)
    target_link_libraries(infilfs-phase1-api PRIVATE infilfs_posix)
    target_compile_definitions(infilfs-phase1-api PRIVATE _GNU_SOURCE)
    infilfs_enable_warnings(infilfs-phase1-api)

    add_test(NAME infilfs-smoke
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/smoke.sh ${CMAKE_BINARY_DIR})
    add_test(NAME infilfs-portable-storage
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/portable-core.sh ${CMAKE_BINARY_DIR})
    add_test(NAME infilfs-phase2-crash
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/phase2-crash.sh ${CMAKE_BINARY_DIR})
    add_test(NAME infilfs-phase3-integrity
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/phase3-integrity.sh ${CMAKE_BINARY_DIR})

    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FUSE3 QUIET IMPORTED_TARGET fuse3)
    endif()

    if(FUSE3_FOUND)
        add_executable(infilfs-fuse fuse/infilfs_fuse.c)
        target_link_libraries(infilfs-fuse PRIVATE infilfs_posix PkgConfig::FUSE3)
        target_compile_definitions(infilfs-fuse PRIVATE _GNU_SOURCE)
        infilfs_enable_warnings(infilfs-fuse)
    else()
        message(STATUS "libfuse3 development files not found; Linux FUSE adapter will not be built")
    endif()

    install(TARGETS mkfs.infilfs infilfs-inspect infilfs-tool infilfs-scrub
            RUNTIME DESTINATION bin)
    if(TARGET infilfs-fuse)
        install(TARGETS infilfs-fuse RUNTIME DESTINATION bin)
    endif()
endif()
