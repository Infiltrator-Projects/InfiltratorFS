# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(InfiltratorFS VERSION 0.18.0 LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(INFILTRATR_COMMON_REQUIRED_VERSION "1.11.0")
set(INFILTRATR_COMMON_REQUIRED_COMMIT
    "6c1a6c239e51dcf7946b6303a9bad639e8455a17")

# InfiltratorFS consumes the same Common contract as the other Infiltrator
# projects. Prefer a parent-provided/installed 1.11.0 package, then the pinned
# submodule. A source checkout without initialised submodules still remains
# buildable by fetching the exact immutable Common commit.
if(NOT TARGET InfiltratrCommon::Portable)
    find_package(InfiltratrCommon ${INFILTRATR_COMMON_REQUIRED_VERSION} EXACT
                 CONFIG QUIET)
endif()
if(NOT TARGET InfiltratrCommon::Portable)
    set(INFILTRATR_COMMON_BUILD_TESTS OFF CACHE BOOL
        "Build Infiltratr Common tests as part of InfiltratorFS" FORCE)
    set(INFILTRATR_COMMON_WARNINGS_AS_ERRORS OFF CACHE BOOL
        "Treat vendored Infiltratr Common warnings as errors" FORCE)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/infiltratr-common/CMakeLists.txt")
        add_subdirectory(src/infiltratr-common EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        FetchContent_Declare(infiltratr_common
            GIT_REPOSITORY https://github.com/The-First-Infiltrator/Infiltrator-Libraries.git
            GIT_TAG ${INFILTRATR_COMMON_REQUIRED_COMMIT}
            GIT_SHALLOW FALSE)
        FetchContent_MakeAvailable(infiltratr_common)
    endif()
endif()
if(NOT TARGET InfiltratrCommon::Portable OR
   NOT TARGET InfiltratrCommon::Common)
    message(FATAL_ERROR
        "Infiltratr Common ${INFILTRATR_COMMON_REQUIRED_VERSION} is required")
endif()

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
    src/forensic.c
    src/volume.c
    src/format_volume.c
)
target_include_directories(infilfs_core PUBLIC include)
target_link_libraries(infilfs_core PUBLIC InfiltratrCommon::Portable)
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

add_executable(infilfs-paged-metadata tests/paged-metadata.c)
target_link_libraries(infilfs-paged-metadata PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-paged-metadata)

add_executable(infilfs-namespace-links tests/symlinks.c)
target_link_libraries(infilfs-namespace-links PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-namespace-links)

add_executable(infilfs-snapshots tests/snapshots.c)
target_link_libraries(infilfs-snapshots PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-snapshots)

add_executable(infilfs-online-scrub tests/online-scrub.c)
target_link_libraries(infilfs-online-scrub PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-online-scrub)

add_executable(infilfs-large-files tests/large-files.c)
target_link_libraries(infilfs-large-files PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-large-files)

add_executable(infilfs-eof-extent-promotion tests/eof-extent-promotion.c)
target_link_libraries(infilfs-eof-extent-promotion PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-eof-extent-promotion)

add_executable(infilfs-device-open tests/device-open.c)
target_link_libraries(infilfs-device-open PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-device-open)

add_executable(infilfs-forensic-api tests/forensic-api.c)
target_link_libraries(infilfs-forensic-api PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-forensic-api)

enable_testing()
add_test(NAME infilfs-checksum-known-answer COMMAND infilfs-checksum-test)
add_test(NAME infilfs-format-conformance COMMAND infilfs-format-conformance)
add_test(NAME infilfs-volume-conformance COMMAND infilfs-volume-conformance)
add_test(NAME infilfs-hardening-conformance COMMAND infilfs-hardening-conformance)
add_test(NAME infilfs-open-hardening COMMAND infilfs-open-hardening)
add_test(NAME infilfs-063-hardening COMMAND infilfs-063-hardening)
add_test(NAME infilfs-inline-files COMMAND infilfs-inline-files)
add_test(NAME infilfs-reflink COMMAND infilfs-reflink)
add_test(NAME infilfs-paged-metadata COMMAND infilfs-paged-metadata)
add_test(NAME infilfs-namespace-links COMMAND infilfs-namespace-links)
add_test(NAME infilfs-snapshots COMMAND infilfs-snapshots)
add_test(NAME infilfs-online-scrub COMMAND infilfs-online-scrub)
add_test(NAME infilfs-large-files COMMAND infilfs-large-files)
add_test(NAME infilfs-eof-extent-promotion COMMAND infilfs-eof-extent-promotion)
add_test(NAME infilfs-device-open COMMAND infilfs-device-open)
add_test(NAME infilfs-forensic-api COMMAND infilfs-forensic-api)

if(WIN32)
    add_library(infilfs_win32 STATIC
        src/platform/win32_io.c
        src/platform/win32_partition_io.c)
    target_include_directories(infilfs_win32 PUBLIC include)
    target_link_libraries(infilfs_win32 PUBLIC infilfs_core bcrypt)
    infilfs_enable_warnings(infilfs_win32)

    add_executable(infilfs-win32-storage tests/win32-storage.c)
    target_link_libraries(infilfs-win32-storage PRIVATE infilfs_win32)
    infilfs_enable_warnings(infilfs-win32-storage)
    add_test(NAME infilfs-win32-storage COMMAND infilfs-win32-storage)

    add_executable(infilfs-win32-partition-region tests/win32-partition-region.c)
    target_link_libraries(infilfs-win32-partition-region PRIVATE infilfs_win32)
    infilfs_enable_warnings(infilfs-win32-partition-region)
    add_test(NAME infilfs-win32-partition-region COMMAND infilfs-win32-partition-region)

    configure_file(
        tools/windows/infiltratorfs-windows-version.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/infiltratorfs-windows-version.rc
        @ONLY)

    add_executable(infiltratorfs-windows WIN32
        tools/windows/infiltratorfs-windows-entry.c
        ${CMAKE_CURRENT_BINARY_DIR}/infiltratorfs-windows-version.rc)
    target_link_libraries(infiltratorfs-windows PRIVATE
        infilfs_win32 comdlg32 shell32 ole32)
    target_compile_definitions(infiltratorfs-windows PRIVATE
        INFILFS_VERSION_W=L\"${PROJECT_VERSION}\")
    if(MSVC)
        target_link_options(infiltratorfs-windows PRIVATE
            "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")
    endif()
    infilfs_enable_warnings(infiltratorfs-windows)
    set_target_properties(infiltratorfs-windows PROPERTIES
        OUTPUT_NAME "InfiltratorFS-Windows-${PROJECT_VERSION}")
endif()

# Linux/POSIX is the first adapter and test host. The core library and disk
# format do not depend on this adapter. Linux mounting is performed solely by
# the native kernel VFS driver from 0.17.0 onward.
if(UNIX)
    add_library(infilfs_posix STATIC src/platform/posix_io.c)
    target_include_directories(infilfs_posix PUBLIC include)
    target_link_libraries(infilfs_posix PUBLIC
        infilfs_core InfiltratrCommon::Common)
    target_compile_definitions(infilfs_posix PRIVATE _GNU_SOURCE)
    infilfs_enable_warnings(infilfs_posix)

    add_executable(mkfs.infilfs tools/mkfs.infilfs.c)
    add_executable(infilfs-inspect tools/infilfs-inspect.c)
    add_executable(infilfs-tool tools/infilfs-tool.c)
    add_executable(infilfs-scrub tools/infilfs-scrub.c)
    add_executable(infilfs-forensic tools/infilfs-forensic.c)
    target_compile_definitions(mkfs.infilfs PRIVATE
        INFILFS_IMPLEMENTATION_VERSION=\"${PROJECT_VERSION}\")
    foreach(tool_target IN ITEMS mkfs.infilfs infilfs-inspect infilfs-tool infilfs-scrub infilfs-forensic)
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
    add_test(NAME infilfs-forensic-scan
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/forensic-scan.sh ${CMAKE_BINARY_DIR})
    add_test(NAME infilfs-manager-shell-syntax
        COMMAND bash -n ${CMAKE_SOURCE_DIR}/tools/infiltratorfs-manager)
    add_test(NAME infilfs-manager-helper-shell-syntax
        COMMAND bash -n ${CMAKE_SOURCE_DIR}/tools/infiltratorfs-manager-helper)
    add_test(NAME infilfs-manager-partition-discovery
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/manager-partition-discovery.sh)
    add_test(NAME infilfs-system-utilities
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/system-utilities.sh)
    add_test(NAME infilfs-desktop-integration
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/desktop-integration.sh
                     ${CMAKE_BINARY_DIR})

    install(TARGETS mkfs.infilfs infilfs-inspect infilfs-tool infilfs-scrub infilfs-forensic
            RUNTIME DESTINATION bin)
    install(PROGRAMS tools/infiltratorfs-manager DESTINATION bin)
    install(PROGRAMS tools/infiltratorfs-manager-helper
            DESTINATION lib/infiltratorfs)
    install(PROGRAMS tools/mount.infiltratorfs tools/fsck.infiltratorfs
            DESTINATION sbin)
    install(FILES packaging/infiltratorfs-manager.desktop
            DESTINATION share/applications)
    install(FILES packaging/59-infiltratorfs.rules
            DESTINATION lib/udev/rules.d)
endif()
