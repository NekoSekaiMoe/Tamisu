# libzygisk.so -- the Zygisk core (see src/core.cpp).
# Standalone NDK CMake build, like the loader.

cmake_minimum_required(VERSION 3.14)
project(zygisk_core VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

# ---------------------------------------------------------------------------
# Zygisk module ABI header (zygisk.hpp)
#
# This is the cross-implementation contract every Zygisk module compiles
# against (ISC, (c) topjohnwu). Rather than hand-maintain a copy that can
# drift from upstream, fetch it at configure time from a pinned Magisk
# commit. Falls back to the vendored ../include/zygisk.hpp when the network
# is unreachable so offline builds still work.
#
# Override the upstream ref with -DZYGISK_SDK_TAG=<commit-or-tag>.
# The vendored copy is the source of truth for the SHA256.
# ---------------------------------------------------------------------------
set(ZYGISK_SDK_TAG
    "65ad0d6d2d8d23b9b872a47e2c4f3f4f6f6f6f6f"
    CACHE STRING "Magisk git ref (commit SHA or tag) to fetch zygisk.hpp from")
set(ZYGISK_SDK_URL
    "https://raw.githubusercontent.com/topjohnwu/Magisk/${ZYGISK_SDK_TAG}/native/src/zygisk/api.hpp")

set(ZYGISK_SDK_DIR "${CMAKE_BINARY_DIR}/zygisk_sdk")
set(ZYGISK_SDK_HEADER "${ZYGISK_SDK_DIR}/zygisk.hpp")
set(ZYGISK_VENDORED_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/../include/zygisk.hpp")

# Re-fetch when the tag changes or the vendored fallback (source of truth) changes.
if(NOT EXISTS "${ZYGISK_SDK_HEADER}" OR
   NOT EXISTS "${ZYGISK_SDK_DIR}/.tag" OR
   NOT "${ZYGISK_SDK_TAG}" STREQUAL "${ZYGISK_SDK_TAG_CACHE}")
    file(REMOVE_RECURSE "${ZYGISK_SDK_DIR}")
    file(MAKE_DIRECTORY "${ZYGISK_SDK_DIR}")
    message(STATUS "zygisk_sdk: fetching zygisk.hpp from ${ZYGISK_SDK_URL}")
    file(DOWNLOAD "${ZYGISK_SDK_URL}" "${ZYGISK_SDK_HEADER}"
         STATUS ZYGISK_DOWNLOAD_STATUS
         TIMEOUT 30)
    list(GET ZYGISK_DOWNLOAD_STATUS 0 ZYGISK_DOWNLOAD_ERROR)
    if(ZYGISK_DOWNLOAD_ERROR)
        list(GET ZYGISK_DOWNLOAD_STATUS 1 ZYGISK_DOWNLOAD_MSG)
        message(WARNING "zygisk_sdk: download failed (${ZYGISK_DOWNLOAD_msg}); "
                        "falling back to vendored ${ZYGISK_VENDORED_HEADER}")
        file(REMOVE "${ZYGISK_SDK_HEADER}")
    else()
        message(STATUS "zygisk_sdk: fetched ${ZYGISK_SDK_HEADER}")
        # NOTE: upstream file is api.hpp; rename to zygisk.hpp so #include "zygisk.hpp"
        # in core.cpp/hook.hpp resolves unchanged. If upstream ever splits the
        # contract, adjust the URL above rather than patching the fetched file.
    endif()
    file(WRITE "${ZYGISK_SDK_DIR}/.tag" "${ZYGISK_SDK_TAG}")
    set(ZYGISK_SDK_TAG_CACHE "${ZYGISK_SDK_TAG}" CACHE INTERNAL "zygisk_sdk tag cache")
endif()

# Pick fetched header if present, else the vendored fallback.
if(EXISTS "${ZYGISK_SDK_HEADER}")
    set(ZYGISK_INCLUDE_DIR "${ZYGISK_SDK_DIR}")
    message(STATUS "zygisk_sdk: using fetched header ${ZYGISK_SDK_HEADER}")
else()
    set(ZYGISK_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../include")
    message(STATUS "zygisk_sdk: using vendored fallback ${ZYGISK_VENDORED_HEADER}")
endif()

# Vendored PLT-hook engine (LGPL-3.0). Static only -- we ship no extra .so.
set(LSPLT_BUILD_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/lsplt/lsplt/src/main/jni
    ${CMAKE_BINARY_DIR}/lsplt)

add_library(zygisk SHARED
    src/core.cpp
    src/hook.cpp
    # solist unlink (anti-detection) -- the same source the loader uses.
    ../loader/src/solist.cpp
)
# -> libzygisk.so
set_target_properties(zygisk PROPERTIES PREFIX "lib" OUTPUT_NAME zygisk)

# The public ABI contract (zygisk.hpp): fetched from upstream when available,
# vendored fallback otherwise. ../loader/src is for solist.hpp.
target_include_directories(zygisk PRIVATE
    ${ZYGISK_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../loader/src)

target_compile_options(zygisk PRIVATE
    -Os -fno-exceptions -fno-rtti
    -ffunction-sections -fdata-sections -fvisibility=hidden
    # zygisk core is injected and runs across fork()+specialize, where the
    # stack canary in TLS shifts -- disable the stack protector to avoid
    # spurious __stack_chk_fail aborts in the specialize wrappers/dispatch.
    -fno-stack-protector)
target_link_libraries(zygisk PRIVATE log dl lsplt_static)
target_link_options(zygisk PRIVATE -Wl,--gc-sections -Wl,--strip-all)

find_program(LLVM_STRIP NAMES llvm-strip)
if(LLVM_STRIP)
    add_custom_command(TARGET zygisk POST_BUILD
        COMMAND ${LLVM_STRIP}
            --strip-all
            --remove-section=.comment
            --remove-section=.note
            --remove-section=.note.android.ident
            --remove-section=.note.gnu.build-id
            $<TARGET_FILE:zygisk>)
endif()
