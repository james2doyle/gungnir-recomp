cmake_minimum_required(VERSION 3.22)

# Materialises fetch-based dependencies into a machine-local directory.
# Run with: cmake -DGUNGNIR_DEPS_DIR=<dir> -P cmake/Deps.cmake
#
# Pins must match dependencies.json; tests/test_android_app.py checks the
# version, URL and SHA-256 against the ledger.

if(NOT DEFINED GUNGNIR_DEPS_DIR OR GUNGNIR_DEPS_DIR STREQUAL "")
    message(FATAL_ERROR "GUNGNIR_DEPS_DIR is required")
endif()

set(SDL3_VERSION "3.4.18")
set(SDL3_ARCHIVE "SDL3-${SDL3_VERSION}.tar.gz")
set(SDL3_URL "https://github.com/libsdl-org/SDL/releases/download/release-${SDL3_VERSION}/${SDL3_ARCHIVE}")
set(SDL3_SHA256 "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3")
set(SDL3_DIR "${GUNGNIR_DEPS_DIR}/sdl3-src")
set(SDL3_STAMP "${GUNGNIR_DEPS_DIR}/sdl3-${SDL3_VERSION}.stamp")

if(EXISTS "${SDL3_STAMP}" AND EXISTS "${SDL3_DIR}/CMakeLists.txt")
    message(STATUS "SDL3 ${SDL3_VERSION}: cached at ${SDL3_DIR}")
    return()
endif()

file(MAKE_DIRECTORY "${GUNGNIR_DEPS_DIR}")
set(archive "${GUNGNIR_DEPS_DIR}/${SDL3_ARCHIVE}")
if(EXISTS "${archive}")
    file(SHA256 "${archive}" existing_sha)
    if(NOT existing_sha STREQUAL SDL3_SHA256)
        message(STATUS "SDL3 ${SDL3_VERSION}: cached archive checksum mismatch, removing")
        file(REMOVE "${archive}")
    endif()
endif()

if(NOT EXISTS "${archive}")
    if(GUNGNIR_OFFLINE)
        message(FATAL_ERROR "SDL3 ${SDL3_VERSION}: archive missing or invalid in offline mode")
    endif()
    message(STATUS "SDL3 ${SDL3_VERSION}: fetching ${SDL3_URL}")
    file(DOWNLOAD "${SDL3_URL}" "${archive}"
         EXPECTED_HASH SHA256=${SDL3_SHA256}
         TLS_VERIFY ON
         STATUS download_status)
    list(GET download_status 0 download_code)
    if(NOT download_code EQUAL 0)
        list(GET download_status 1 download_message)
        message(FATAL_ERROR "SDL3 download failed: ${download_message}")
    endif()
endif()

set(extract_dir "${GUNGNIR_DEPS_DIR}/sdl3-extract")
file(REMOVE_RECURSE "${extract_dir}" "${SDL3_DIR}")
file(MAKE_DIRECTORY "${extract_dir}")
file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${extract_dir}")
if(NOT EXISTS "${extract_dir}/SDL3-${SDL3_VERSION}/CMakeLists.txt")
    message(FATAL_ERROR "SDL3 archive layout unexpected (no SDL3-${SDL3_VERSION}/CMakeLists.txt)")
endif()
file(RENAME "${extract_dir}/SDL3-${SDL3_VERSION}" "${SDL3_DIR}")
file(REMOVE_RECURSE "${extract_dir}")
file(WRITE "${SDL3_STAMP}" "${SDL3_VERSION}\n")
message(STATUS "SDL3 ${SDL3_VERSION}: ${SDL3_DIR}")
