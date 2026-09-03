# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(InfiltratorFS VERSION 0.18.39 LANGUAGES C)
# Release qualification anchor: 0.18.39

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
            GIT_REPOSITORY https://github.com/Infiltrator-Projects/Infiltrator-Libraries.git
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

# Per-extent compression is a portable-format feature. Pin LZ4 so Linux,
# Windows and future adapters encode the same byte stream without relying on
# a distribution-specific shared library version.
include(FetchContent)
set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LZ4_BUNDLED_MODE ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
FetchContent_Declare(infilfs_lz4
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG v1.10.0
    SOURCE_SUBDIR build/cmake
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(infilfs_lz4)

# Release 0.18.34 standardises InfiltratorFS desktop applications on the
# same bundled MB Corpo typography on Linux and Windows. Keep the archive immutable and verify
# every extracted face so a build cannot silently fall back to system fonts.
set(INFILFS_FONT_ARCHIVE
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/mb-corpo-fonts.tar.xz")
set(INFILFS_FONT_ARCHIVE_SHA256
    "bdb6063f838a7fab22b4d6b412170640c69511df53aa3dfa9a4ea8431c9d8274")
if(NOT EXISTS "${INFILFS_FONT_ARCHIVE}")
    message(FATAL_ERROR "Required InfiltratorFS MB Corpo font archive is missing")
endif()
file(SHA256 "${INFILFS_FONT_ARCHIVE}" INFILFS_FONT_ARCHIVE_ACTUAL_SHA256)
if(NOT INFILFS_FONT_ARCHIVE_ACTUAL_SHA256 STREQUAL INFILFS_FONT_ARCHIVE_SHA256)
    message(FATAL_ERROR "InfiltratorFS MB Corpo font archive hash mismatch")
endif()

set(INFILFS_FONT_DIR "${CMAKE_CURRENT_BINARY_DIR}/infiltratorfs-fonts")
file(REMOVE_RECURSE "${INFILFS_FONT_DIR}")
file(MAKE_DIRECTORY "${INFILFS_FONT_DIR}")
file(ARCHIVE_EXTRACT INPUT "${INFILFS_FONT_ARCHIVE}"
                     DESTINATION "${INFILFS_FONT_DIR}")
set(INFILFS_FONT_A_COND_REGULAR
    "${INFILFS_FONT_DIR}/mb_corpo_a_cond_regular.ttf")
set(INFILFS_FONT_S_BOLD
    "${INFILFS_FONT_DIR}/mb_corpo_s_bold.ttf")
set(INFILFS_FONT_S_REGULAR
    "${INFILFS_FONT_DIR}/mb_corpo_s_regular.ttf")
set(INFILFS_FONT_FILES
    "${INFILFS_FONT_A_COND_REGULAR}"
    "${INFILFS_FONT_S_BOLD}"
    "${INFILFS_FONT_S_REGULAR}")

function(infilfs_verify_font path expected_sha256)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Required InfiltratorFS MB Corpo font is missing: ${path}")
    endif()
    file(SHA256 "${path}" actual_sha256)
    if(NOT actual_sha256 STREQUAL expected_sha256)
        get_filename_component(font_name "${path}" NAME)
        message(FATAL_ERROR "InfiltratorFS MB Corpo font hash mismatch: ${font_name}")
    endif()
endfunction()

infilfs_verify_font(
    "${INFILFS_FONT_A_COND_REGULAR}"
    "c8bcd7e1a7d71169b38491d9b7c1ffe7ba7b46e888f0c1219931343a47bc0e05")
infilfs_verify_font(
    "${INFILFS_FONT_S_BOLD}"
    "d37ea986e2344d83390f94f170e6272b56efd00bfec808afe8314c4ca45d43b4")
infilfs_verify_font(
    "${INFILFS_FONT_S_REGULAR}"
    "94ede6629443c03d4362dcef425fb3ff520be5d654370021a34e81286804465c")

function(infilfs_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
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
target_link_libraries(infilfs_core PUBLIC InfiltratrCommon::Portable lz4_static)
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

add_executable(infilfs-compression-codec tests/compression-codec.c)
target_link_libraries(infilfs-compression-codec PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-compression-codec)

add_executable(infilfs-compression-extents tests/compression-extents.c)
target_link_libraries(infilfs-compression-extents PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-compression-extents)

add_executable(infilfs-compression-qualification tests/compression-qualification.c)
target_link_libraries(infilfs-compression-qualification PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-compression-qualification)

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

add_executable(infilfs-free-extent-index tests/free-extent-index.c)
target_link_libraries(infilfs-free-extent-index PRIVATE infilfs_core)
infilfs_enable_warnings(infilfs-free-extent-index)

enable_testing()
add_test(NAME infilfs-checksum-known-answer COMMAND infilfs-checksum-test)
add_test(NAME infilfs-format-conformance COMMAND infilfs-format-conformance)
add_test(NAME infilfs-volume-conformance COMMAND infilfs-volume-conformance)
add_test(NAME infilfs-hardening-conformance COMMAND infilfs-hardening-conformance)
add_test(NAME infilfs-open-hardening COMMAND infilfs-open-hardening)
add_test(NAME infilfs-063-hardening COMMAND infilfs-063-hardening)
add_test(NAME infilfs-inline-files COMMAND infilfs-inline-files)
add_test(NAME infilfs-reflink COMMAND infilfs-reflink)
add_test(NAME infilfs-compression-codec COMMAND infilfs-compression-codec)
add_test(NAME infilfs-compression-extents COMMAND infilfs-compression-extents)
add_test(NAME infilfs-compression-qualification COMMAND infilfs-compression-qualification)
add_test(NAME infilfs-paged-metadata COMMAND infilfs-paged-metadata)
add_test(NAME infilfs-namespace-links COMMAND infilfs-namespace-links)
add_test(NAME infilfs-snapshots COMMAND infilfs-snapshots)
add_test(NAME infilfs-online-scrub COMMAND infilfs-online-scrub)
add_test(NAME infilfs-large-files COMMAND infilfs-large-files)
add_test(NAME infilfs-eof-extent-promotion COMMAND infilfs-eof-extent-promotion)
add_test(NAME infilfs-device-open COMMAND infilfs-device-open)
add_test(NAME infilfs-forensic-api COMMAND infilfs-forensic-api)
add_test(NAME infilfs-free-extent-index COMMAND infilfs-free-extent-index)

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

    set(INFILFS_FONT_A_COND_REGULAR_RC "${INFILFS_FONT_A_COND_REGULAR}")
    set(INFILFS_FONT_S_BOLD_RC "${INFILFS_FONT_S_BOLD}")
    set(INFILFS_FONT_S_REGULAR_RC "${INFILFS_FONT_S_REGULAR}")
    configure_file(
        tools/windows/infiltratorfs-windows-fonts.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/infiltratorfs-windows-fonts.rc
        @ONLY)

    configure_file(
        tools/windows/infiltratorfs-windows-version.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/infiltratorfs-windows-version.rc
        @ONLY)

    add_library(infilfs_windows_bridge STATIC
        tools/windows/infiltratorfs-windows-bridge.c)
    target_include_directories(infilfs_windows_bridge PUBLIC tools/windows)
    target_link_libraries(infilfs_windows_bridge PUBLIC
        infilfs_win32 shell32 ole32)
    infilfs_enable_warnings(infilfs_windows_bridge)

    add_executable(infiltratorfs-windows WIN32
        tools/windows/infiltratorfs-windows-entry.c
        ${CMAKE_CURRENT_BINARY_DIR}/infiltratorfs-windows-version.rc
        ${CMAKE_CURRENT_BINARY_DIR}/infiltratorfs-windows-fonts.rc)
    target_include_directories(infiltratorfs-windows PRIVATE tools/windows)
    target_link_libraries(infiltratorfs-windows PRIVATE
        infilfs_windows_bridge comdlg32 comctl32 dwmapi uxtheme advapi32)
    target_compile_definitions(infiltratorfs-windows PRIVATE
        INFILFS_VERSION_W=L\"${PROJECT_VERSION}\")

    add_executable(infilfs-win32-projfs-bridge
        tests/win32-projfs-bridge.c)
    target_link_libraries(infilfs-win32-projfs-bridge PRIVATE
        infilfs_windows_bridge)
    infilfs_enable_warnings(infilfs-win32-projfs-bridge)
    if(MSVC)
        target_link_options(infiltratorfs-windows PRIVATE
            "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\""
            "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/tools/windows/infiltratorfs-windows.manifest")
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
    add_executable(infilfs-optimize tools/infilfs-optimize.c)
    add_executable(infiltratorfs-resize tools/infiltratorfs-resize.c)
    add_executable(infiltratorfs-quota tools/infiltratorfs-quota.c)
    target_include_directories(infilfs-optimize PRIVATE kernel)
    target_include_directories(infiltratorfs-resize PRIVATE kernel)
    target_include_directories(infiltratorfs-quota PRIVATE kernel)
    target_compile_definitions(mkfs.infilfs PRIVATE
        INFILFS_IMPLEMENTATION_VERSION=\"${PROJECT_VERSION}\")
    foreach(tool_target IN ITEMS mkfs.infilfs infilfs-inspect infilfs-tool infilfs-scrub infilfs-forensic infilfs-optimize infiltratorfs-resize infiltratorfs-quota)
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
    add_test(NAME infilfs-resize
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/resize.sh ${CMAKE_BINARY_DIR})
    add_test(NAME infilfs-forensic-scan
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/forensic-scan.sh ${CMAKE_BINARY_DIR})
    add_test(NAME infilfs-current-format-tree
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/current-format-tree.sh ${CMAKE_BINARY_DIR})
    add_test(NAME infilfs-manager-python-syntax
        COMMAND python3 -c
            "import ast,pathlib; ast.parse(pathlib.Path(r'${CMAKE_SOURCE_DIR}/tools/infiltratorfs-manager').read_text(encoding='utf-8'))")
    add_test(NAME infilfs-ui-font-policy
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/ui-font-policy.sh
                     ${CMAKE_SOURCE_DIR})
    add_test(NAME infilfs-manager-helper-shell-syntax
        COMMAND bash -n ${CMAKE_SOURCE_DIR}/tools/infiltratorfs-manager-helper)
    add_test(NAME infilfs-manager-partition-discovery
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/manager-partition-discovery.sh)
    add_test(NAME infilfs-system-utilities
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/system-utilities.sh)
    add_test(NAME infilfs-desktop-integration
        COMMAND bash ${CMAKE_SOURCE_DIR}/tests/desktop-integration.sh
                     ${CMAKE_BINARY_DIR})

    install(TARGETS mkfs.infilfs infilfs-inspect infilfs-tool infilfs-scrub infilfs-forensic infilfs-optimize infiltratorfs-resize infiltratorfs-quota
            RUNTIME DESTINATION bin)
    install(PROGRAMS tools/infiltratorfs-manager tools/mkfs.infiltratorfs
            DESTINATION bin)
    install(PROGRAMS tools/infiltratorfs-manager-helper
                     packaging/infiltratorfs-os-integration
            DESTINATION lib/infiltratorfs)
    install(PROGRAMS tools/mount.infiltratorfs tools/fsck.infiltratorfs
            DESTINATION sbin)
    install(FILES packaging/infiltratorfs-manager.desktop
            DESTINATION share/applications)
    # Nemo already gets a stock "Format" action from Mintstick.  Ship our
    # partition-safe replacement beside the integration helper instead of as a
    # second visible action; the helper diverts the stock action and installs
    # this one in its place on supported Linux Mint systems.
    install(FILES packaging/infiltratorfs-format-partition.nemo_action
            DESTINATION lib/infiltratorfs)
    install(FILES ${INFILFS_FONT_FILES}
            DESTINATION share/infiltratorfs/fonts)
    install(FILES packaging/59-infiltratorfs.rules
            DESTINATION lib/udev/rules.d)
endif()
