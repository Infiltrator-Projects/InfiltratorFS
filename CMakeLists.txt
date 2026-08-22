# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(InfiltratorFS VERSION 0.9.4 LANGUAGES C)

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
    src/status.c
    src/storage.c
    src/fs.c
    src/format_volume.c
    src/utf8.c
    src/volume.c
)
target_include_directories(infilfs_core PUBLIC include)
infilfs_enable_warnings(infilfs_core)

if(WIN32)
    add_library(infilfs_win32 STATIC src/platform/win32_io.c)
    target_include_directories(infilfs_win32 PUBLIC include)
    target_link_libraries(infilfs_win32 PUBLIC infilfs_core bcrypt)
    infilfs_enable_warnings(infilfs_win32)

    add_executable(infilfs-win32-storage-test tests/win32-storage.c)
    target_link_libraries(infilfs-win32-storage-test PRIVATE infilfs_win32)
    infilfs_enable_warnings(infilfs-win32-storage-test)

    add_executable(InfiltratorFS-Windows WIN32 tools/windows/infiltratorfs-windows.c)
    target_link_libraries(InfiltratorFS-Windows PRIVATE infilfs_win32 shell32 comdlg32 ole32)
    infilfs_enable_warnings(InfiltratorFS-Windows)
    if(MSVC)
        target_link_options(InfiltratorFS-Windows PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
    endif()
else()
    add_library(infilfs_posix STATIC src/platform/posix_io.c)
    target_include_directories(infilfs_posix PUBLIC include)
    target_link_libraries(infilfs_posix PUBLIC infilfs_core)
    infilfs_enable_warnings(infilfs_posix)

    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FUSE3 QUIET fuse3)
    endif()
    if(FUSE3_FOUND)
        add_executable(infilfs-fuse fuse/infilfs_fuse.c)
        target_include_directories(infilfs-fuse PRIVATE ${FUSE3_INCLUDE_DIRS})
        target_compile_options(infilfs-fuse PRIVATE ${FUSE3_CFLAGS_OTHER})
        target_link_directories(infilfs-fuse PRIVATE ${FUSE3_LIBRARY_DIRS})
        target_link_libraries(infilfs-fuse PRIVATE infilfs_posix ${FUSE3_LIBRARIES})
        infilfs_enable_warnings(infilfs-fuse)
    endif()
endif()

add_executable(mkfs.infilfs tools/mkfs.infilfs.c)
add_executable(infilfs-inspect tools/infilfs-inspect.c)
add_executable(infilfs-tool tools/infilfs-tool.c)
if(WIN32)
    target_link_libraries(mkfs.infilfs PRIVATE infilfs_win32)
    target_link_libraries(infilfs-inspect PRIVATE infilfs_win32)
    target_link_libraries(infilfs-tool PRIVATE infilfs_win32)
else()
    target_link_libraries(mkfs.infilfs PRIVATE infilfs_posix)
    target_link_libraries(infilfs-inspect PRIVATE infilfs_posix)
    target_link_libraries(infilfs-tool PRIVATE infilfs_posix)
endif()
infilfs_enable_warnings(mkfs.infilfs)
infilfs_enable_warnings(infilfs-inspect)
infilfs_enable_warnings(infilfs-tool)

include(CTest)
if(BUILD_TESTING)
    add_executable(infilfs-format-conformance tests/format-conformance.c)
    target_link_libraries(infilfs-format-conformance PRIVATE infilfs_core)
    infilfs_enable_warnings(infilfs-format-conformance)
    add_test(NAME format-conformance COMMAND infilfs-format-conformance)

    add_executable(infilfs-volume-conformance tests/volume-conformance.c)
    target_link_libraries(infilfs-volume-conformance PRIVATE infilfs_core)
    infilfs_enable_warnings(infilfs-volume-conformance)
    add_test(NAME volume-conformance COMMAND infilfs-volume-conformance)

    add_executable(infilfs-hardening-conformance tests/hardening-conformance.c)
    target_link_libraries(infilfs-hardening-conformance PRIVATE infilfs_core)
    infilfs_enable_warnings(infilfs-hardening-conformance)
    add_test(NAME hardening-conformance COMMAND infilfs-hardening-conformance)

    add_executable(infilfs-open-hardening tests/open-hardening.c)
    target_link_libraries(infilfs-open-hardening PRIVATE infilfs_core)
    infilfs_enable_warnings(infilfs-open-hardening)
    add_test(NAME open-hardening COMMAND infilfs-open-hardening)

    add_executable(infilfs-crash-recovery tests/crash-recovery.c)
    target_link_libraries(infilfs-crash-recovery PRIVATE infilfs_core)
    infilfs_enable_warnings(infilfs-crash-recovery)
    add_test(NAME crash-recovery COMMAND infilfs-crash-recovery)

    add_executable(infilfs-sparse-files tests/sparse-files.c)
    target_link_libraries(infilfs-sparse-files PRIVATE infilfs_core)
    infilfs_enable_warnings(infilfs-sparse-files)
    add_test(NAME sparse-files COMMAND infilfs-sparse-files)

    add_executable(infilfs-inline-files tests/inline-files.c)
    target_link_libraries(infilfs-inline-files PRIVATE infilfs_core)
    infilfs_enable_warnings(infilfs-inline-files)
    add_test(NAME inline-files COMMAND infilfs-inline-files)

    add_executable(infilfs-reflink tests/reflink.c)
    target_link_libraries(infilfs-reflink PRIVATE infilfs_core)
    infilfs_enable_warnings(infilfs-reflink)
    add_test(NAME reflink COMMAND infilfs-reflink)

    add_executable(infilfs-paged-metadata tests/paged-metadata.c)
    target_link_libraries(infilfs-paged-metadata PRIVATE infilfs_core)
    infilfs_enable_warnings(infilfs-paged-metadata)
    add_test(NAME paged-metadata COMMAND infilfs-paged-metadata)

    add_executable(infilfs-deferred-write tests/deferred-write.c)
    target_link_libraries(infilfs-deferred-write PRIVATE infilfs_core)
    infilfs_enable_warnings(infilfs-deferred-write)
    add_test(NAME deferred-write COMMAND infilfs-deferred-write)

    add_executable(infilfs-portable-core tests/portable-core.c)
    target_link_libraries(infilfs-portable-core PRIVATE infilfs_core)
    infilfs_enable_warnings(infilfs-portable-core)
    add_test(NAME portable-core COMMAND infilfs-portable-core)

    if(WIN32)
        add_test(NAME win32-storage COMMAND infilfs-win32-storage-test)
    else()
        add_executable(infilfs-phase1-api tests/phase1-api.c)
        target_link_libraries(infilfs-phase1-api PRIVATE infilfs_posix)
        infilfs_enable_warnings(infilfs-phase1-api)
        add_test(NAME phase1-api COMMAND infilfs-phase1-api)

        add_test(NAME smoke
                 COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.sh ${CMAKE_BINARY_DIR})
    endif()
endif()
