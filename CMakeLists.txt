# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(InfiltratorFS VERSION 0.8.1 LANGUAGES C)

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
    src/utf8.c
    src/fs.c
    src/volume.c
    src/format_volume.c
)
target_include_directories(infilfs_core PUBLIC include)
infilfs_enable_warnings(infilfs_core)

add_executable(infilfs-checksum-test tests/checksum-test.c)
target_link_libraries(infilfs-checksum-test PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-checksum-test)

add_executable(infilfs-portable-core tests/portable-core.c)
target_link_libraries(infilfs-portable-core PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-portable-core)

add_executable(infilfs-format-conformance tests/format-conformance.c)
target_link_libraries(infilfs-format-conformance PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-format-conformance)

add_executable(infilfs-volume-conformance tests/volume-conformance.c)
target_link_libraries(infilfs-volume-conformance PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-volume-conformance)

add_executable(infilfs-hardening-conformance tests/hardening-conformance.c)
target_link_libraries(infilfs-hardening-conformance PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-hardening-conformance)

add_executable(infilfs-open-hardening tests/open-hardening.c)
target_link_libraries(infilfs-open-hardening PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-open-hardening)

add_executable(infilfs-063-hardening tests/0.6.3-hardening.c)
target_link_libraries(infilfs-063-hardening PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-063-hardening)

add_executable(infilfs-inline-files tests/inline-files.c)
target_link_libraries(infilfs-inline-files PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-inline-files)

add_executable(infilfs-reflink tests/reflink.c)
target_link_libraries(infilfs-reflink PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-reflink)

enable_testing()
add_test(NAME infilfs-checksum-known-answer COMMAND infilfs-checksum-test)
add_test(NAME infilfs-format-conformance COMMAND infilfs-format-conformance)
add_test(NAME infilfs-volume-conformance COMMAND infilfs-volume-conformance)
add_test(NAME infilfs-hardening-conformance COMMAND infilfs-hardening-conformance)
add_test(NAME infilfs-open-hardening COMMAND infilfs-open-hardening)
add_test(NAME infilfs-063-hardening COMMAND infilfs-063-hardening)
add_test(NAME infilfs-inline-files COMMAND infilfs-inline-files)
add_test(NAME infilfs-reflink COMMAND infilfs-reflink)

if(WIN32)
    add_library(infilfs_win32 STATIC src/platform/win32_io.c)
    target_include_directories(infilfs_win32 PUBLIC include)
    target_link_libraries(infilfs_win32 PUBLIC infilfs_core bcrypt)
    infilfs_enable_warnings(infilfs_win32)

    add_executable(infilfs-win32-storage tests/win32-storage.c)
    target_link_libraries(infilfs-win32-storage PRIVATE infilfs_win32)
    infilfs_enable_warnings(infilfs-win32-storage)
    add_test(NAME infilfs-win32-storage COMMAND infilfs-win32-storage)

    add_executable(infiltratorfs-windows WIN32
        tools/windows/infiltratorfs-windows.c)
    target_link_libraries(infiltratorfs-windows PRIVATE
        infilfs_win32 comdlg32 shell32 ole32)
    if(MSVC)
        target_link_options(infiltratorfs-windows PRIVATE
            "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")
    endif()
    infilfs_enable_warnings(infiltratorfs-windows)
    set_target_properties(infiltratorfs-windows PROPERTIES
        OUTPUT_NAME "InfiltratorFS-Windows")
endif()

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

    add_executable(infilfs-posix-locking tests/posix-locking.c)
    target_link_libraries(infilfs-posix-locking PRIVATE infilfs_posix)
    target_compile_definitions(infilfs-posix-locking PRIVATE _GNU_SOURCE)
    infilfs_enable_warnings(infilfs-posix-locking)

    add_test(NAME infilfs-posix-locking COMMAND infilfs-posix-locking)

    add_test(NAME infilfs-smoke
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/smoke.sh ${CMAKE_BINARY_DIR})
    add_test(NAME infilfs-portable-storage
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/portable-core.sh ${CMAKE_BINARY_DIR})
    add_test(NAME infilfs-phase2-crash
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/phase2-crash.sh ${CMAKE_BINARY_DIR})
    add_test(NAME infilfs-phase3-integrity
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/phase3-integrity.sh ${CMAKE_BINARY_DIR})
    add_test(NAME infilfs-sparse-files
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/sparse-files.sh ${CMAKE_BINARY_DIR})
    add_test(NAME infilfs-manager-shell-syntax
        COMMAND bash -n ${CMAKE_SOURCE_DIR}/tools/infiltratorfs-manager)
    add_test(NAME infilfs-manager-helper-shell-syntax
        COMMAND bash -n ${CMAKE_SOURCE_DIR}/tools/infiltratorfs-manager-helper)

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
    install(PROGRAMS tools/infiltratorfs-manager DESTINATION bin)
    install(PROGRAMS tools/infiltratorfs-manager-helper
            DESTINATION lib/infiltratorfs)
    install(FILES packaging/infiltratorfs-manager.desktop
            DESTINATION share/applications)
endif()
