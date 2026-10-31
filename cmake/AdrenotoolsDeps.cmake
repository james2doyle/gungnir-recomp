cmake_minimum_required(VERSION 3.22)

# Materialises the Android driver-loader dependencies into a machine-local
# cache. Run with:
#   cmake -DGUNGNIR_DEPS_DIR=<dir> -P cmake/AdrenotoolsDeps.cmake
#
# Pins must match dependencies.json. The libadrenotools git submodule is not
# present in GitHub commit archives, so liblinkernsbypass is fetched and
# composed into lib/linkernsbypass before the Android build configures it.

if(NOT DEFINED GUNGNIR_DEPS_DIR OR GUNGNIR_DEPS_DIR STREQUAL "")
    message(FATAL_ERROR "GUNGNIR_DEPS_DIR is required")
endif()

function(gungnir_fetch_archive name url sha archive_name)
    set(archive "${GUNGNIR_DEPS_DIR}/${archive_name}")
    if(EXISTS "${archive}")
        file(SHA256 "${archive}" existing_sha)
        if(NOT existing_sha STREQUAL "${sha}")
            message(STATUS "${name}: cached archive checksum mismatch, removing")
            file(REMOVE "${archive}")
        endif()
    endif()
    if(NOT EXISTS "${archive}")
        if(GUNGNIR_OFFLINE)
            message(FATAL_ERROR "${name}: archive missing or invalid in offline mode")
        endif()
        message(STATUS "${name}: fetching ${url}")
        file(DOWNLOAD "${url}" "${archive}"
             EXPECTED_HASH SHA256=${sha}
             TLS_VERIFY ON
             STATUS download_status)
        list(GET download_status 0 download_code)
        if(NOT download_code EQUAL 0)
            list(GET download_status 1 download_message)
            message(FATAL_ERROR "${name} download failed: ${download_message}")
        endif()
    endif()
endfunction()

function(gungnir_extract_single name archive destination expected)
    if(EXISTS "${destination}/${expected}")
        message(STATUS "${name}: cached at ${destination}")
        return()
    endif()
    set(extract_dir "${GUNGNIR_DEPS_DIR}/${name}-extract")
    file(REMOVE_RECURSE "${extract_dir}" "${destination}")
    file(MAKE_DIRECTORY "${extract_dir}")
    file(ARCHIVE_EXTRACT INPUT "${GUNGNIR_DEPS_DIR}/${archive}" DESTINATION "${extract_dir}")
    file(GLOB entries "${extract_dir}/*")
    list(LENGTH entries entry_count)
    if(NOT entry_count EQUAL 1)
        message(FATAL_ERROR "${name}: archive layout unexpected")
    endif()
    list(GET entries 0 top)
    file(RENAME "${top}" "${destination}")
    file(REMOVE_RECURSE "${extract_dir}")
    if(NOT EXISTS "${destination}/${expected}")
        message(FATAL_ERROR "${name}: missing ${expected}")
    endif()
    message(STATUS "${name}: ${destination}")
endfunction()

set(ADRENOTOOLS_ARCHIVE "8fae8ce254dfc1344527e05301e43f37dea2df80.tar.gz")
set(ADRENOTOOLS_URL "https://github.com/bylaws/libadrenotools/archive/8fae8ce254dfc1344527e05301e43f37dea2df80.tar.gz")
set(ADRENOTOOLS_SHA256 "ceffce971676d4cfdf348a082df06fc92a1dca6d95bea892a480d63f200961cb")
set(ADRENOTOOLS_DIR "${GUNGNIR_DEPS_DIR}/libadrenotools-src")

set(LINKER_ARCHIVE "aa3975893d83ef1bc84c321ec60c65fbf1287887.tar.gz")
set(LINKER_URL "https://github.com/bylaws/liblinkernsbypass/archive/aa3975893d83ef1bc84c321ec60c65fbf1287887.tar.gz")
set(LINKER_SHA256 "da1128c8aa771c4d24766b53a47822d0717baa5c536ca8491220402942b80638")
set(LINKER_DIR "${ADRENOTOOLS_DIR}/lib/linkernsbypass")

file(MAKE_DIRECTORY "${GUNGNIR_DEPS_DIR}")

gungnir_fetch_archive("libadrenotools" "${ADRENOTOOLS_URL}" "${ADRENOTOOLS_SHA256}" "${ADRENOTOOLS_ARCHIVE}")
gungnir_extract_single("libadrenotools" "${ADRENOTOOLS_ARCHIVE}" "${ADRENOTOOLS_DIR}" "CMakeLists.txt")

if(NOT EXISTS "${LINKER_DIR}/CMakeLists.txt")
    gungnir_fetch_archive("liblinkernsbypass" "${LINKER_URL}" "${LINKER_SHA256}" "${LINKER_ARCHIVE}")
    set(linker_extract "${GUNGNIR_DEPS_DIR}/liblinkernsbypass-extract")
    file(REMOVE_RECURSE "${linker_extract}")
    file(MAKE_DIRECTORY "${linker_extract}")
    file(ARCHIVE_EXTRACT INPUT "${GUNGNIR_DEPS_DIR}/${LINKER_ARCHIVE}" DESTINATION "${linker_extract}")
    file(GLOB linker_entries "${linker_extract}/*")
    list(GET linker_entries 0 linker_top)
    file(MAKE_DIRECTORY "${LINKER_DIR}")
    file(COPY "${linker_top}/" DESTINATION "${LINKER_DIR}")
    file(REMOVE_RECURSE "${linker_extract}")
    message(STATUS "liblinkernsbypass: composed into ${LINKER_DIR}")
else()
    message(STATUS "liblinkernsbypass: cached at ${LINKER_DIR}")
endif()
if(NOT EXISTS "${LINKER_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "liblinkernsbypass: CMakeLists.txt missing after composition")
endif()
