cmake_minimum_required(VERSION 3.16)
project(asngn C)

# ── Options ─────────────────────────────────────────────────────────────────
option(ASNGN_BUILD_TUI   "Build the asngn terminal application"   ON)
option(ASNGN_BUILD_MCP   "Build the asngn-mcp server"             ON)
option(ASNGN_BUILD_TESTS "Build unit/integration tests"           ON)
option(ASNGN_NO_THREADS  "Single-threaded build (asngn_tick)"     OFF)
option(ASNGN_SANITIZERS  "Enable ASan/UBSan"                      OFF)
option(ASNGN_WITH_LLAMA  "Build the llama.cpp inference backend"  ON)

# Sibling working trees (pinned deps). By default
# they are expected next to this repository, as in the asterism workspace.
set(ASNGN_ASPER_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/../asterism-asper"
    CACHE PATH "Path to the asterism-asper working tree")
set(ASNGN_ASTOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../asterism-astools"
    CACHE PATH "Path to the asterism-astools working tree")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Strict flags apply to asngn translation units only (deps use their own).
if(MSVC)
  set(ASNGN_C_FLAGS /W4 /WX)
else()
  set(ASNGN_C_FLAGS -Wall -Wextra -Wpedantic -Werror)
endif()

if(ASNGN_SANITIZERS AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ── Dependencies ────────────────────────────────────────────────────────────
foreach(dep_dir "${ASNGN_ASPER_DIR}" "${ASNGN_ASTOOLS_DIR}")
  if(NOT EXISTS "${dep_dir}/CMakeLists.txt")
    message(FATAL_ERROR "sibling missing: ${dep_dir} — clone the asterism "
            "siblings next to this repository or set ASNGN_ASPER_DIR / "
            "ASNGN_ASTOOLS_DIR")
  endif()
endforeach()

# xCDN-C: one copy for the whole process — astools' patched tree (string
# escapes, parser depth cap, OOM-safe growth; see astools docs/XCDN-MODS.md).
# Apply astools' local fix if this checkout has not been configured yet
# (detected by the buggy pattern, so a patched or fixed-upstream tree skips).
set(ASNGN_XCDN_DIR "${ASNGN_ASTOOLS_DIR}/deps/xcdn-c")
if(NOT EXISTS "${ASNGN_XCDN_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "${ASNGN_XCDN_DIR} missing — run: git submodule update "
          "--init inside ${ASNGN_ASTOOLS_DIR}")
endif()
file(READ "${ASNGN_XCDN_DIR}/src/lexer.c" ASNGN_XCDN_LEXER)
string(FIND "${ASNGN_XCDN_LEXER}" "strbuf_push(&sb, 'n')" ASNGN_XCDN_BUGGY)
if(NOT ASNGN_XCDN_BUGGY EQUAL -1)
  execute_process(
    COMMAND git apply
            "${ASNGN_ASTOOLS_DIR}/deps/patches/0001-xcdn-c-string-escapes-and-oom.patch"
    WORKING_DIRECTORY "${ASNGN_XCDN_DIR}"
    RESULT_VARIABLE ASNGN_PATCH_RC)
  if(NOT ASNGN_PATCH_RC EQUAL 0)
    message(FATAL_ERROR "failed to apply astools' xcdn-c patch")
  endif()
  message(STATUS "applied astools' local xcdn-c fixes")
endif()
unset(ASNGN_XCDN_LEXER)
# xcdn-c registers its add_test entries unconditionally, so its targets must
# be buildable whenever our tests run (same approach as the siblings).
if(ASNGN_BUILD_TESTS)
  add_subdirectory("${ASNGN_XCDN_DIR}" deps-build/xcdn-c)
else()
  add_subdirectory("${ASNGN_XCDN_DIR}" deps-build/xcdn-c EXCLUDE_FROM_ALL)
endif()

# libasper: memory sibling. Its guarded CMakeLists skips its own
# xcdn-c because the target already exists; it still adds llama.cpp, which
# asngn's model runtime links as well.
set(ASPER_BUILD_MCP   OFF CACHE BOOL "" FORCE)
set(ASPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASPER_NO_THREADS  ${ASNGN_NO_THREADS} CACHE BOOL "" FORCE)
set(ASPER_WITH_LLAMA  ${ASNGN_WITH_LLAMA} CACHE BOOL "" FORCE)
# llama.cpp enables compiler caching by default and emits a warning when no
# cache frontend is installed. Disable that optional integration silently on
# minimal build hosts; it does not affect generated artifacts.
find_program(ASNGN_COMPILER_CACHE NAMES ccache sccache)
if(NOT ASNGN_COMPILER_CACHE)
  set(GGML_CCACHE OFF CACHE BOOL "Use a compiler cache for llama.cpp" FORCE)
endif()
unset(ASNGN_COMPILER_CACHE CACHE)
add_subdirectory("${ASNGN_ASPER_DIR}" deps-build/asper EXCLUDE_FROM_ALL)

# libastools: tools sibling. The std tool suite builds so the engine
# has a ready registry (build/packages); server/CLI/plugin extras stay off.
set(ASTOOLS_BUILD_MCP    OFF CACHE BOOL "" FORCE)
set(ASTOOLS_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
set(ASTOOLS_BUILD_CHECK  OFF CACHE BOOL "" FORCE)
set(ASTOOLS_BUILD_PLUGIN OFF CACHE BOOL "" FORCE)
set(ASTOOLS_BUILD_STD    ON  CACHE BOOL "" FORCE)
set(ASTOOLS_NO_THREADS   ${ASNGN_NO_THREADS} CACHE BOOL "" FORCE)
add_subdirectory("${ASNGN_ASTOOLS_DIR}" deps-build/astools)

# The pinned xCDN source uses strtok_r.  In strict C99 mode glibc exposes it
# only when a POSIX feature level is selected by the consuming build.
if(UNIX AND NOT APPLE)
  target_compile_definitions(xcdn PRIVATE _POSIX_C_SOURCE=200809L)
  target_compile_definitions(asper_obj PRIVATE _POSIX_C_SOURCE=200809L)
  target_compile_definitions(astools_obj PRIVATE _POSIX_C_SOURCE=200809L)
  target_compile_definitions(astools-std PRIVATE _POSIX_C_SOURCE=200809L)
endif()

# GCC 15 added a few diagnostics that the current pinned sibling revisions
# intentionally trigger.  Keep their own -Werror policy intact for every
# other warning while allowing this top-level build to consume those pinned
# revisions cleanly.  These targets are owned by the sibling projects.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
  target_compile_options(asper_obj PRIVATE -Wno-format-truncation)
  target_compile_options(astools_obj PRIVATE -Wno-format-truncation)
  target_compile_options(astools-std PRIVATE
    -Wno-format-truncation -Wno-alloc-size)
endif()

# ── libasngn ─────────────────────────────────────────────────────────────────
set(ASNGN_SOURCES
  src/util.c
  src/time.c
  src/uuid.c
  src/sha256.c
  src/log.c
  src/xutil.c
  src/stream.c
  src/config.c
  src/session.c
  src/ledger.c
  src/tokens.c
  src/models.c
  src/models_llama.c
  src/context.c
  src/fold.c
  src/digest.c
  src/embedcache.c
  src/cache.c
  src/toolcache.c
  src/route.c
  src/detail.c
  src/grammar.c
  src/steps.c
  src/loop.c
  src/safety.c
  src/redact.c
  src/judge.c
  src/telemetry.c
  src/siblings.c
  src/worker.c
  src/api.c
  src/introspect.c
  src/os_common.c
)
if(WIN32)
  list(APPEND ASNGN_SOURCES src/os_win32.c)
else()
  list(APPEND ASNGN_SOURCES src/os_posix.c)
endif()

add_library(asngn_obj OBJECT ${ASNGN_SOURCES})
target_include_directories(asngn_obj
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${ASNGN_ASPER_DIR}/include
          ${ASNGN_ASTOOLS_DIR}/include)
target_compile_options(asngn_obj PRIVATE ${ASNGN_C_FLAGS})
target_link_libraries(asngn_obj PUBLIC xcdn)
if(UNIX AND NOT APPLE)
  target_compile_definitions(asngn_obj PRIVATE _POSIX_C_SOURCE=200809L)
endif()
if(ASNGN_NO_THREADS)
  target_compile_definitions(asngn_obj PUBLIC ASNGN_NO_THREADS)
endif()
if(ASNGN_WITH_LLAMA)
  target_compile_definitions(asngn_obj PRIVATE ASNGN_WITH_LLAMA)
  target_link_libraries(asngn_obj PRIVATE llama)
endif()

add_library(asngn_static STATIC $<TARGET_OBJECTS:asngn_obj>)
set_target_properties(asngn_static PROPERTIES OUTPUT_NAME asngn)
add_library(asngn_shared SHARED $<TARGET_OBJECTS:asngn_obj>)
set_target_properties(asngn_shared PROPERTIES OUTPUT_NAME asngn)

foreach(tgt asngn_static asngn_shared)
  target_include_directories(${tgt} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(${tgt} PUBLIC xcdn asper_static astools_static)
  if(UNIX AND NOT APPLE)
    target_compile_definitions(${tgt} PUBLIC _POSIX_C_SOURCE=200809L)
  endif()
  if(ASNGN_NO_THREADS)
    # Consumers that include internal headers (the test suite and sibling
    # fixtures) must see the same os_mutex/os_rwlock layouts as libasngn.
    target_compile_definitions(${tgt} PUBLIC ASNGN_NO_THREADS)
  endif()
  if(ASNGN_WITH_LLAMA)
    target_link_libraries(${tgt} PUBLIC llama)
  endif()
  if(NOT WIN32 AND NOT ASNGN_NO_THREADS)
    find_package(Threads REQUIRED)
    target_link_libraries(${tgt} PUBLIC Threads::Threads)
  endif()
  if(NOT WIN32)
    target_link_libraries(${tgt} PUBLIC ${CMAKE_DL_LIBS} m)
  endif()
endforeach()

# ── asngn (TUI + headless CLI) ───────────────────────────────────────────────
if(ASNGN_BUILD_TUI AND WIN32)
  message(WARNING "ASNGN_BUILD_TUI disabled: the terminal UI currently has "
                  "a POSIX backend only; libasngn and asngn-mcp are supported")
  set(ASNGN_BUILD_TUI OFF)
endif()
if(ASNGN_BUILD_TUI)
  if(ASNGN_NO_THREADS)
    message(FATAL_ERROR "the TUI requires the threaded build; "
            "configure with -DASNGN_BUILD_TUI=OFF")
  endif()
  add_executable(asngn
    tui/main.c
    tui/term.c
    tui/draw.c
    tui/theme.c
    tui/input.c
    tui/panes.c
    tui/modal.c
    tui/chat.c
  )
  target_include_directories(asngn PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tui)
  target_compile_options(asngn PRIVATE ${ASNGN_C_FLAGS})
  target_link_libraries(asngn PRIVATE asngn_static)
endif()

# ── asngn-mcp ───────────────────────────────────────────────────────────────
if(ASNGN_BUILD_MCP)
  add_executable(asngn-mcp mcp/main.c src/json.c)
  target_include_directories(asngn-mcp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/mcp)
  target_compile_options(asngn-mcp PRIVATE ${ASNGN_C_FLAGS})
  target_link_libraries(asngn-mcp PRIVATE asngn_static)
endif()

# ── Tests ───────────────────────────────────────────────────────────────────
if(ASNGN_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
