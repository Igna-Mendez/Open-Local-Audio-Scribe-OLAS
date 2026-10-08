cmake_minimum_required(VERSION 3.20)

project(OLAS VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR
        "Debug builds are ~5x slower. Reconfigure with -DCMAKE_BUILD_TYPE=Release")
endif()

# ---------------------------------------------------------------------------
# Moonshine
# ---------------------------------------------------------------------------
#
# Moonshine ships a prebuilt SDK per platform on its GitHub Releases, and that
# is what this project consumes. The Linux build does the same.
#
# Why not build Moonshine from source via FetchContent? Because that route no
# longer works against current Moonshine. Upstream restructured the repository
# after v0.0.65: the public headers (moonshine-cpp.h / moonshine-c-api.h) moved
# out of the source tree entirely, and the v0.0.67-v0.0.69 tags reference
# cpp-annote/src/community1_ort_embedded.cpp, a file those tags do not contain.
# Building from a tag therefore fails at generate time, and the newer tags have
# no headers to include. The released SDK has both, and it tracks latest.
#
# The tarball is ~26 MB and contains:
#   include/moonshine-cpp.h, moonshine-c-api.h
#   lib/moonshine.lib, moonshine-utils.lib, bin-tokenizer.lib, ort-utils.lib,
#       onnxruntime.lib, onnxruntime.dll
#
# Override the whole thing with -DMOONSHINE_SDK_DIR=<path> to use an SDK you
# already have, or -DMOONSHINE_SDK_URL=<url> to fetch from elsewhere.

set(MOONSHINE_VERSION "latest" CACHE STRING
    "Moonshine release tag, or 'latest' to follow the newest release")

if(MOONSHINE_VERSION STREQUAL "latest")
    set(MOONSHINE_SDK_URL
        "https://github.com/moonshine-ai/moonshine/releases/latest/download/moonshine-voice-windows-x86_64.tar.gz"
        CACHE STRING "Moonshine Windows SDK tarball URL")
else()
    set(MOONSHINE_SDK_URL
        "https://github.com/moonshine-ai/moonshine/releases/download/${MOONSHINE_VERSION}/moonshine-voice-windows-x86_64.tar.gz"
        CACHE STRING "Moonshine Windows SDK tarball URL")
endif()

set(MOONSHINE_SDK_DIR "${CMAKE_BINARY_DIR}/moonshine-sdk" CACHE PATH
    "Directory holding the extracted Moonshine SDK (include/ and lib/)")

# A cached SDK is reused unless something says otherwise, so a build tree made
# months ago would keep that release's headers and libraries forever. Record
# which release was fetched and re-fetch when Moonshine publishes a new one.
# Re-run with -DMOONSHINE_REFRESH=1 to force it, or delete moonshine-sdk/.
set(MOONSHINE_REFRESH OFF CACHE BOOL "Re-download the Moonshine SDK")
set(_sdk_ver_file "${MOONSHINE_SDK_DIR}/.sdk-version")

set(_sdk_resolved_version "")
set(_need_sdk FALSE)
if(NOT EXISTS "${MOONSHINE_SDK_DIR}/include/moonshine-cpp.h")
    set(_need_sdk TRUE)
elseif(MOONSHINE_REFRESH)
    message(STATUS "OLAS: SDK refresh requested (-DMOONSHINE_REFRESH=1)")
    set(_need_sdk TRUE)
elseif(MOONSHINE_VERSION STREQUAL "latest")
    # Ask GitHub only when we already have an SDK, to avoid a needless call on
    # a fresh tree. Offline or rate-limited, keep what we have.
    file(DOWNLOAD
        "https://api.github.com/repos/moonshine-ai/moonshine/releases/latest"
        "${CMAKE_BINARY_DIR}/_moonshine_latest.json"
        STATUS _rel_status TLS_VERIFY ON)
    list(GET _rel_status 0 _rel_code)
    if(_rel_code EQUAL 0)
        file(READ "${CMAKE_BINARY_DIR}/_moonshine_latest.json" _rel_json)
        string(REGEX MATCH "\"tag_name\"[ \t]*:[ \t]*\"([^\"]*)\"" _m "${_rel_json}")
        set(_remote "${CMAKE_MATCH_1}")
        set(_local "")
        if(EXISTS "${_sdk_ver_file}")
            file(READ "${_sdk_ver_file}" _local)
            string(STRIP "${_local}" _local)
        endif()
        if(NOT _remote STREQUAL "" AND NOT _remote STREQUAL _local)
            message(STATUS "OLAS: new Moonshine release "
                           "${_local} -> ${_remote}")
            set(_need_sdk TRUE)
        endif()
        set(_sdk_resolved_version "${_remote}")
    endif()
    file(REMOVE "${CMAKE_BINARY_DIR}/_moonshine_latest.json")
endif()

if(_need_sdk)
    message(STATUS "OLAS: fetching the Moonshine SDK")
    message(STATUS "      ${MOONSHINE_SDK_URL}")

    set(_sdk_tar "${CMAKE_BINARY_DIR}/moonshine-sdk.tar.gz")
    file(DOWNLOAD "${MOONSHINE_SDK_URL}" "${_sdk_tar}"
         STATUS _dl_status
         SHOW_PROGRESS)
    list(GET _dl_status 0 _dl_code)
    if(NOT _dl_code EQUAL 0)
        list(GET _dl_status 1 _dl_msg)
        message(FATAL_ERROR
            "Could not download the Moonshine SDK.\n"
            "  URL:   ${MOONSHINE_SDK_URL}\n"
            "  Error: ${_dl_msg}\n"
            "Fetch it manually and pass -DMOONSHINE_SDK_DIR=<extracted dir>.")
    endif()

    file(MAKE_DIRECTORY "${MOONSHINE_SDK_DIR}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf "${_sdk_tar}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        RESULT_VARIABLE _extract_rc)
    if(NOT _extract_rc EQUAL 0)
        message(FATAL_ERROR "Could not extract ${_sdk_tar}")
    endif()

    # The archive has a single top-level directory; flatten it so that
    # include/ and lib/ sit directly under MOONSHINE_SDK_DIR.
    file(GLOB _top "${CMAKE_BINARY_DIR}/moonshine-voice-windows-*")
    if(_top)
        list(GET _top 0 _topdir)
        file(COPY "${_topdir}/" DESTINATION "${MOONSHINE_SDK_DIR}")
        file(REMOVE_RECURSE "${_topdir}")
    endif()
    file(REMOVE "${_sdk_tar}")

    # Remember which release this came from, so a later configure can tell
    # whether it is stale.
    #
    # Pinned to a tag: we know it. "latest" on a fresh tree: the API lookup at
    # the top is skipped (there is nothing to compare against), so look it up
    # now purely to label the download. If that fails, record "unknown" and
    # the next configure will treat the SDK as possibly stale and refresh --
    # which is the safe direction.
    if(NOT MOONSHINE_VERSION STREQUAL "latest")
        set(_sdk_resolved_version "${MOONSHINE_VERSION}")
    elseif(_sdk_resolved_version STREQUAL "")
        file(DOWNLOAD
            "https://api.github.com/repos/moonshine-ai/moonshine/releases/latest"
            "${CMAKE_BINARY_DIR}/_moonshine_latest.json"
            STATUS _rel2_status TLS_VERIFY ON)
        list(GET _rel2_status 0 _rel2_code)
        if(_rel2_code EQUAL 0)
            file(READ "${CMAKE_BINARY_DIR}/_moonshine_latest.json" _rel2_json)
            string(REGEX MATCH "\"tag_name\"[ \t]*:[ \t]*\"([^\"]*)\""
                   _m2 "${_rel2_json}")
            set(_sdk_resolved_version "${CMAKE_MATCH_1}")
        endif()
        file(REMOVE "${CMAKE_BINARY_DIR}/_moonshine_latest.json")
    endif()
    if(_sdk_resolved_version STREQUAL "")
        set(_sdk_resolved_version "unknown")
    endif()
    file(WRITE "${_sdk_ver_file}" "${_sdk_resolved_version}\n")
    message(STATUS "OLAS: Moonshine SDK ${_sdk_resolved_version}")
endif()

set(MOONSHINE_INCLUDE_DIR "${MOONSHINE_SDK_DIR}/include")
set(MOONSHINE_LIB_DIR     "${MOONSHINE_SDK_DIR}/lib")

if(NOT EXISTS "${MOONSHINE_INCLUDE_DIR}/moonshine-cpp.h")
    message(FATAL_ERROR
        "moonshine-cpp.h not found under ${MOONSHINE_INCLUDE_DIR}.\n"
        "The SDK layout may have changed, or MOONSHINE_SDK_DIR points at the "
        "wrong place. Expected include/moonshine-cpp.h next to lib/moonshine.lib.")
endif()
if(NOT EXISTS "${MOONSHINE_LIB_DIR}/moonshine.lib")
    message(FATAL_ERROR "moonshine.lib not found under ${MOONSHINE_LIB_DIR}")
endif()

# Imported target for the SDK, so the rest of this file and the linker only
# need one name.
add_library(moonshine_sdk SHARED IMPORTED GLOBAL)
set_target_properties(moonshine_sdk PROPERTIES
    IMPORTED_IMPLIB        "${MOONSHINE_LIB_DIR}/moonshine.lib"
    IMPORTED_LOCATION      "${MOONSHINE_LIB_DIR}/moonshine.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${MOONSHINE_INCLUDE_DIR}")

message(STATUS "OLAS: Moonshine SDK at ${MOONSHINE_SDK_DIR}")

# The SDK bundles ONNX Runtime; there is no separate mode to choose.
set(ORT_DLL_SOURCE "${MOONSHINE_LIB_DIR}/onnxruntime.dll")
message(STATUS "OLAS: ONNX Runtime provided by the Moonshine SDK (bundled)")

# ---------------------------------------------------------------------------
# Build provenance + update-check metadata
# ---------------------------------------------------------------------------
find_package(Git QUIET)

if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE OLAS_GIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE OLAS_GIT_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

if(NOT OLAS_GIT_SHA)
    set(OLAS_GIT_SHA "unknown")
endif()
if(NOT OLAS_GIT_BRANCH)
    set(OLAS_GIT_BRANCH "unknown")
endif()

string(TIMESTAMP OLAS_BUILD_DATE "%Y-%m-%d %H:%M:%S UTC" UTC)

# GitHub repo whose *releases* are checked for updates. "owner/name" form.
# Leave EMPTY to disable the update check entirely.
set(OLAS_UPDATE_REPO "Igna-Mendez/Open-Local-Audio-Scribe-OLAS" CACHE STRING
    "GitHub repo (owner/name) for update checks; empty to disable")

# ---------------------------------------------------------------------------
# OLAS executable
# ---------------------------------------------------------------------------
add_executable(olas_win WIN32
    src/main_win.cpp
    src/capture.c
    src/win32_ui.cpp
    src/moonshine_engine.cpp
    src/resource_plan.cpp
    src/model_choice.cpp
    src/miniaudio_impl.c
    src/update_check.cpp
)

target_include_directories(olas_win PRIVATE
    src
    "${MOONSHINE_INCLUDE_DIR}"
)

target_compile_definitions(olas_win PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    OLAS_GIT_SHA="${OLAS_GIT_SHA}"
    OLAS_GIT_BRANCH="${OLAS_GIT_BRANCH}"
    OLAS_BUILD_DATE="${OLAS_BUILD_DATE}"
    OLAS_UPDATE_REPO="${OLAS_UPDATE_REPO}"
)

target_link_libraries(olas_win PRIVATE
    comctl32
    ole32
    oleaut32
    uuid
    user32
    gdi32
    advapi32
    shell32
    winmm
    avrt
    dwmapi
    winhttp
)

# The Moonshine SDK bundles ONNX Runtime; linking the SDK is enough.
target_link_libraries(olas_win PRIVATE moonshine_sdk)

# --- Entry point / subsystem -------------------------------------------------
# `add_executable(... WIN32 ...)` sets /SUBSYSTEM:WINDOWS, whose CRT entry
# stub is WinMainCRTStartup (expects WinMain).  We use int main() plus
# CommandLineToArgvW, so retarget the CRT at mainCRTStartup on MSVC.
# (MinGW ships a WinMain shim that calls main, so it's already fine there.)
if(MSVC)
    set_target_properties(olas_win PROPERTIES LINK_FLAGS "/ENTRY:mainCRTStartup")
    target_compile_options(olas_win PRIVATE
        /utf-8
        /O2
        /arch:AVX2
        /fp:fast
        /GL
    )
    set_target_properties(olas_win PROPERTIES
        INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
    )
endif()

if(MINGW)
    target_link_options(olas_win PRIVATE
        -static
        -static-libgcc
        -static-libstdc++
        -mwindows
        -pthread
    )
endif()

# ---------------------------------------------------------------------------
# Post-build: make the build tree runnable in place by putting the runtime
# DLL next to the exe. The SDK ships it under lib/.
# ---------------------------------------------------------------------------
if(WIN32)
    add_custom_command(TARGET olas_win POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ORT_DLL_SOURCE}"
            "$<TARGET_FILE_DIR:olas_win>/onnxruntime.dll"
        COMMENT "Copying onnxruntime.dll next to olas_win.exe"
        VERBATIM
    )
endif()

# ---------------------------------------------------------------------------
# Convenience target: distributable folder
#
# Produces ${CMAKE_BINARY_DIR}/dist/OLAS-win64/ containing everything a user
# needs to run OLAS: the exe, the ONNX Runtime DLL, the launcher .bat, the
# models folder, and the docs. Point a zip tool at this folder.
# ---------------------------------------------------------------------------
set(DIST_DIR "${CMAKE_BINARY_DIR}/dist/OLAS-win64")
set(DIST_ZIP "${CMAKE_BINARY_DIR}/dist/OLAS-win64-${PROJECT_VERSION}.zip")

add_custom_target(dist
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}"
    # Binary
    COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE:olas_win>" "${DIST_DIR}/"
    # Runtime DLL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ORT_DLL_SOURCE}" "${DIST_DIR}/onnxruntime.dll"
    # Models. A release build ships all three so both modes work offline;
    # run setup.ps1 (or tools\fetch-streaming-models.ps1) first. The target
    # still succeeds without them, with a warning, so a build is never
    # blocked by a missing download.
    COMMAND ${CMAKE_COMMAND}
            -DMODELS_SRC=${CMAKE_SOURCE_DIR}/models
            -DDIST_DIR=${DIST_DIR}
            -P ${CMAKE_SOURCE_DIR}/cmake/copy-models.cmake
    # Launcher + docs
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/OLAS.bat"   "${DIST_DIR}/OLAS.bat"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/README.md"    "${DIST_DIR}/README.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/PATCHNOTES.md" "${DIST_DIR}/PATCHNOTES.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/LICENSE"    "${DIST_DIR}/LICENSE"
    # Runtime tuning (transcription parameters); shipped as a default the
    # user can edit or delete.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/olas-win.conf" "${DIST_DIR}/olas-win.conf"
    DEPENDS olas_win
    COMMENT "Packaging OLAS-win64 distribution folder into ${DIST_DIR}"
    VERBATIM
)

# ---------------------------------------------------------------------------
# Convenience target: build dist, then zip it.
#   cmake --build . --target zip
# -> ${CMAKE_BINARY_DIR}/dist/OLAS-win64-<version>.zip
# ---------------------------------------------------------------------------
add_custom_target(zip
    COMMAND ${CMAKE_COMMAND} -E rm -f "${DIST_ZIP}"
    COMMAND ${CMAKE_COMMAND} -E tar "cfv" "${DIST_ZIP}" --format=zip
            -- "${DIST_DIR}"
    DEPENDS dist
    COMMENT "Zipping distribution folder -> ${DIST_ZIP}"
    VERBATIM
)
