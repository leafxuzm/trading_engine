cmake_minimum_required(VERSION 3.20)
project(trading_engine VERSION 1.0.0 LANGUAGES CXX
        DESCRIPTION "Chronos Trading Engine — HFT pipeline demo")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# ── Compiler flags ───────────────────────────────────────────────
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native -mtune=native")

# ── Dependencies ─────────────────────────────────────────────────
# libchronos-deps provides: fpm, yaml-cpp, gflags, spdlog, nlohmann_json,
# simdjson, Boost, OpenSSL, libzmq — all via FetchContent.
# It is a PUBLIC repo; anyone can clone it.
if(EXISTS "${CMAKE_SOURCE_DIR}/../libchronos-deps/CMakeLists.txt")
    add_subdirectory("${CMAKE_SOURCE_DIR}/../libchronos-deps"
                     "${CMAKE_BINARY_DIR}/chronos_deps")
else()
    message(FATAL_ERROR
        "libchronos-deps not found at ../libchronos-deps.\n"
        "Clone it first:\n"
        "  git clone https://github.com/leafxuzm/libchronos-deps.git")
endif()

# ── libchronos — headers bundled, library auto-downloaded ─────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(FindChronos)

if(NOT CHRONOS_FOUND)
    message(FATAL_ERROR "libchronos not found — see cmake/FindChronos.cmake")
endif()

# ── Trading engine executable ────────────────────────────────────
add_executable(trading_engine
    src/main.cpp
)

target_include_directories(trading_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(trading_engine PRIVATE
    chronos
)

target_compile_features(trading_engine PRIVATE cxx_std_20)

# ── Demo log generator tool ───────────────────────────────────────
add_executable(generate_demo_logs
    tools/generate_demo_logs.cpp
)

target_link_libraries(generate_demo_logs PRIVATE chronos)
target_compile_features(generate_demo_logs PRIVATE cxx_std_20)

# ── Configuration summary ────────────────────────────────────────
message(STATUS "")
message(STATUS "trading_engine Configuration Summary:")
message(STATUS "  Version:        ${PROJECT_VERSION}")
message(STATUS "  Build type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard:   C++${CMAKE_CXX_STANDARD}")
if(CHRONOS_BUILT_FROM_SOURCE)
    message(STATUS "  libchronos:     built from source (dev mode)")
else()
    message(STATUS "  libchronos:     pre-built ${CHRONOS_LIBRARY}")
endif()
message(STATUS "  Headers:        ${CHRONOS_INCLUDE_DIR}")
message(STATUS "")
