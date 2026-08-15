# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(InfiltratorFS VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

add_library(infilfs_core STATIC
    src/checksum.c
    src/io.c
    src/fs.c
    src/volume.c
)
target_include_directories(infilfs_core PUBLIC include)
target_compile_options(infilfs_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(mkfs.infilfs tools/mkfs.infilfs.c)
target_link_libraries(mkfs.infilfs PRIVATE infilfs_core)
target_compile_options(mkfs.infilfs PRIVATE -Wall -Wextra -Wpedantic)

add_executable(infilfs-inspect tools/infilfs-inspect.c)
target_link_libraries(infilfs-inspect PRIVATE infilfs_core)
target_compile_options(infilfs-inspect PRIVATE -Wall -Wextra -Wpedantic)

add_executable(infilfs-tool tools/infilfs-tool.c)
target_link_libraries(infilfs-tool PRIVATE infilfs_core)
target_compile_options(infilfs-tool PRIVATE -Wall -Wextra -Wpedantic)

add_executable(infilfs-phase1-api tests/phase1-api.c)
target_link_libraries(infilfs-phase1-api PRIVATE infilfs_core)
target_compile_options(infilfs-phase1-api PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_test(NAME infilfs-smoke
    COMMAND bash ${CMAKE_SOURCE_DIR}/tests/smoke.sh ${CMAKE_BINARY_DIR})

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FUSE3 QUIET IMPORTED_TARGET fuse3)
endif()

if(FUSE3_FOUND)
    add_executable(infilfs-fuse fuse/infilfs_fuse.c)
    target_link_libraries(infilfs-fuse PRIVATE infilfs_core PkgConfig::FUSE3)
    target_compile_options(infilfs-fuse PRIVATE -Wall -Wextra -Wpedantic)
else()
    message(STATUS "libfuse3 development files not found; infilfs-fuse will not be built")
endif()

install(TARGETS mkfs.infilfs infilfs-inspect infilfs-tool RUNTIME DESTINATION bin)
if(TARGET infilfs-fuse)
    install(TARGETS infilfs-fuse RUNTIME DESTINATION bin)
endif()
