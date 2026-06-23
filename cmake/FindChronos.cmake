# FindChronos.cmake — locate or download libchronos.a
#
# Searches in order:
#   1. CHRONOS_LIBRARY cmake/environment variable (explicit path)
#   2. ${CMAKE_SOURCE_DIR}/lib/libchronos.a (bundled in repo)
#   3. ${CMAKE_SOURCE_DIR}/../libchronos (dev mode — build from source)
#   4. GitHub Releases download (pre-built binary for this platform)
#
# On success, sets:
#   CHRONOS_FOUND        — TRUE
#   CHRONOS_LIBRARY      — path to libchronos.a
#   CHRONOS_INCLUDE_DIR  — path to bundled headers
#   chronos              — imported STATIC library target

set(CHRONOS_VERSION "1.0.0")

# ── Headers are always bundled ─────────────────────────────────────
set(CHRONOS_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../include")

# ── 1. Explicit override ───────────────────────────────────────────
if(DEFINED ENV{CHRONOS_LIBRARY} AND NOT "$ENV{CHRONOS_LIBRARY}" STREQUAL "")
    set(CHRONOS_LIBRARY "$ENV{CHRONOS_LIBRARY}")
elseif(CHRONOS_LIBRARY)
    # Already set via -DCHRONOS_LIBRARY=...
endif()

# ── 2. Bundled in repo ─────────────────────────────────────────────
if(NOT CHRONOS_LIBRARY)
    set(_bundled "${CMAKE_CURRENT_LIST_DIR}/../lib/libchronos.a")
    if(EXISTS "${_bundled}")
        set(CHRONOS_LIBRARY "${_bundled}")
        message(STATUS "Using bundled libchronos.a: ${_bundled}")
    endif()
endif()

# ── 3. Dev mode — build from sibling source ────────────────────────
if(NOT CHRONOS_LIBRARY)
    set(_src_dir "${CMAKE_CURRENT_LIST_DIR}/../../libchronos")
    if(EXISTS "${_src_dir}/CMakeLists.txt")
        message(STATUS "Dev mode: building libchronos from source (${_src_dir})")
        add_subdirectory("${_src_dir}" "${CMAKE_BINARY_DIR}/libchronos")
        # libchronos target is now available; headers come from the source tree.
        # Override CHRONOS_INCLUDE_DIR to use the source (not bundled) headers
        # so devs editing headers see their changes immediately.
        set(CHRONOS_INCLUDE_DIR "${_src_dir}/include")
        set(CHRONOS_FOUND TRUE)
        set(CHRONOS_BUILT_FROM_SOURCE TRUE)
        return()
    endif()
endif()

# ── 4. Download from GitHub Releases ───────────────────────────────
if(NOT CHRONOS_LIBRARY)
    # Detect platform tuple
    execute_process(
        COMMAND uname -s
        OUTPUT_VARIABLE _sysname OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(
        COMMAND uname -m
        OUTPUT_VARIABLE _machine OUTPUT_STRIP_TRAILING_WHITESPACE)

    if(_sysname STREQUAL "Darwin")
        if(_machine STREQUAL "arm64")
            set(_platform "macos-arm64")
        else()
            set(_platform "macos-x86_64")
        endif()
    elseif(_sysname STREQUAL "Linux")
        set(_platform "linux-x86_64")
    else()
        set(_platform "unknown")
    endif()

    set(_release_url
        "https://github.com/leafxuzm/libchronos/releases/download/v${CHRONOS_VERSION}/libchronos-${_platform}.a")
    set(_archive "${CMAKE_BINARY_DIR}/libchronos-${_platform}.a")

    message(STATUS "Downloading libchronos.a for ${_platform}...")
    file(DOWNLOAD "${_release_url}" "${_archive}"
         STATUS _dl_status
         TIMEOUT 60
         INACTIVITY_TIMEOUT 30)

    list(GET _dl_status 0 _dl_code)
    list(GET _dl_status 1 _dl_msg)

    if(_dl_code EQUAL 0)
        set(CHRONOS_LIBRARY "${_archive}")
        message(STATUS "Downloaded libchronos.a (${_platform})")
    else()
        message(FATAL_ERROR
            "Cannot find libchronos.a.\n"
            "  Download failed (${_dl_msg}): ${_release_url}\n"
            "  Options:\n"
            "    1. Place libchronos.a in ${CMAKE_SOURCE_DIR}/lib/\n"
            "    2. Set CHRONOS_LIBRARY=/path/to/libchronos.a\n"
            "    3. Clone libchronos alongside trading_engine for dev build\n"
            "       git clone <libchronos> ../libchronos")
    endif()
endif()

# ── Create imported target ─────────────────────────────────────────
if(NOT TARGET chronos)
    add_library(chronos STATIC IMPORTED)
    set_target_properties(chronos PROPERTIES
        IMPORTED_LOCATION "${CHRONOS_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${CHRONOS_INCLUDE_DIR}"
    )
    # Link libchronos-deps so consumers get third-party headers + libraries
    target_link_libraries(chronos INTERFACE chronos_deps)
endif()

set(CHRONOS_FOUND TRUE)
