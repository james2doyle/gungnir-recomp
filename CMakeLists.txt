cmake_minimum_required(VERSION 3.22)
project(gungnir-recomp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Vendored third_party (see third_party/yakumo/PROVENANCE.md) ----------
add_library(yakumo STATIC
    third_party/yakumo/src/elf32.cpp
    third_party/yakumo/src/guest_memory.cpp
    third_party/yakumo/src/iso_image.cpp
    third_party/yakumo/src/param_sfo.cpp
    third_party/yakumo/src/utf8_path.cpp)
target_include_directories(yakumo PUBLIC
    third_party/yakumo/include
    third_party/yakumo/src)

# --- Intake reader tool (T3) ---------------------------------------------
# Subcommands: iso-extract, elf-inspect, sfo-info. Line-oriented protocol on
# stdout; named errors ("ERR <NAME> ...") on stderr. The orchestrator is
# tools/intake/intake.py.
add_executable(intake_tool tools/intake/intake_tool.cpp)
target_link_libraries(intake_tool PRIVATE yakumo)
set_target_properties(intake_tool PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools/intake)
