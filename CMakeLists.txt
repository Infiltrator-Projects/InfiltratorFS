# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(InfiltratorFS VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

add_library(infilfs_core STATIC
    src/checksum.c
    src/io.c
    src/fs.c
)
target_include_directories(infilfs_core PUBLIC include)
target_compile_options(infilfs_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(mkfs.infilfs tools/mkfs.infilfs.c)
target_link_libraries(mkfs.infilfs PRIVATE infilfs_core)
target_compile_options(mkfs.infilfs PRIVATE -Wall -Wextra -Wpedantic)

add_executable(infilfs-inspect tools/infilfs-inspect.c)
target_link_libraries(infilfs-inspect PRIVATE infilfs_core)
target_compile_options(infilfs-inspect PRIVATE -Wall -Wextra -Wpedantic)

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

install(TARGETS mkfs.infilfs infilfs-inspect RUNTIME DESTINATION bin)
if(TARGET infilfs-fuse)
    install(TARGETS infilfs-fuse RUNTIME DESTINATION bin)
endif()
