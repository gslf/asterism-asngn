cmake_minimum_required(VERSION 3.16)
project(asngn C)
if(POLICY CMP0077)
  cmake_policy(SET CMP0077 NEW)
endif()

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
set(ASNGN_ASMODEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../asterism-asmodel"
    CACHE PATH "Path to the asterism-asmodel working tree")

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
  # The C sources use the portable libc string functions; keep MSVC's
  # "unsafe CRT" deprecation notes from tripping /WX.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  set(ASNGN_C_FLAGS -Wall -Wextra -Wpedantic -Werror)
endif()

if(ASNGN_SANITIZERS AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ── Dependencies ────────────────────────────────────────────────────────────
foreach(dep_dir "${ASNGN_ASPER_DIR}" "${ASNGN_ASTOOLS_DIR}" "${ASNGN_ASMODEL_DIR}")
  if(NOT EXISTS "${dep_dir}/CMakeLists.txt")
    message(FATAL_ERROR "sibling missing: ${dep_dir} — clone the asterism "
            "siblings next to this repository or set ASNGN_ASPER_DIR / "
            "ASNGN_ASTOOLS_DIR / ASNGN_ASMODEL_DIR")
  endif()
endforeach()

set(ASMODEL_BUILD_TESTS OFF)
add_subdirectory("${ASNGN_ASMODEL_DIR}" deps-build/asmodel EXCLUDE_FROM_ALL)

# xCDN-C: one copy for the whole process, supplied by astools' pinned
# submodule. Configuration is deliberately read-only: dependency fixes belong
# in the sibling's gitlink and must never be applied to a working tree here.
set(ASNGN_XCDN_DIR "${ASNGN_ASTOOLS_DIR}/deps/xcdn-c")
if(NOT EXISTS "${ASNGN_XCDN_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "${ASNGN_XCDN_DIR} missing — run: git submodule update "
          "--init inside ${ASNGN_ASTOOLS_DIR}")
endif()
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
set(ASPER_BUILD_MCP   OFF)
set(ASPER_BUILD_TESTS OFF)
set(ASPER_NO_THREADS  ${ASNGN_NO_THREADS})
set(ASPER_WITH_LLAMA  ${ASNGN_WITH_LLAMA})
# llama.cpp enables compiler caching by default and emits a warning when no
# cache frontend is installed. Disable that optional integration silently on
# minimal build hosts; it does not affect generated artifacts.
# llama.cpp defaults to shared llama/ggml libraries. Windows executables
# have no rpath, so the test and MCP binaries would need those DLLs copied
# next to them or on PATH; build the dependency stack static instead.
if(WIN32 AND CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
endif()
find_program(ASNGN_COMPILER_CACHE NAMES ccache sccache)
if(NOT ASNGN_COMPILER_CACHE)
  set(GGML_CCACHE OFF)
endif()
unset(ASNGN_COMPILER_CACHE)
add_subdirectory("${ASNGN_ASPER_DIR}" deps-build/asper EXCLUDE_FROM_ALL)

# libastools: tools sibling. The std tool suite builds so the engine
# has a ready registry (build/packages); server/CLI/plugin extras stay off.
set(ASTOOLS_BUILD_MCP    OFF)
set(ASTOOLS_BUILD_TESTS  OFF)
set(ASTOOLS_BUILD_CHECK  OFF)
set(ASTOOLS_BUILD_PLUGIN OFF)
set(ASTOOLS_BUILD_STD    ON)
set(ASTOOLS_NO_THREADS   ${ASNGN_NO_THREADS})
add_subdirectory("${ASNGN_ASTOOLS_DIR}" deps-build/astools)

# The pinned xCDN source uses strtok_r.  In strict C99 mode glibc exposes it
# only when a POSIX feature level is selected by the consuming build.
if(UNIX AND NOT APPLE)
  target_compile_definitions(xcdn PRIVATE _POSIX_C_SOURCE=200809L)
  target_compile_definitions(asper_obj PRIVATE _POSIX_C_SOURCE=200809L)
  target_compile_definitions(astools_obj PRIVATE _POSIX_C_SOURCE=200809L)
  target_compile_definitions(astools-std PRIVATE _POSIX_C_SOURCE=200809L)
endif()

# MSVC has no strtok_r; its strtok_s takes the same arguments in the same
# order, so map the pinned xCDN source onto it.  C4244 (int64_t narrowing)
# fires on the pinned astools revision under /W4 /WX; sibling-owned, silenced
# here like the GCC 15 diagnostics below.
if(MSVC)
  target_compile_definitions(xcdn PRIVATE strtok_r=strtok_s)
  target_compile_options(astools_obj PRIVATE /wd4244)
  # llama.h is not /W4-clean (C4201 nameless struct/union); asper includes
  # it without an MSVC-side pragma guard.
  target_compile_options(asper_obj PRIVATE /wd4201)
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
  src/workspace.c
  src/ledger.c
  src/tokens.c
  src/models.c
  src/models_llama.c
  src/models_openai.c
  src/models_shared.c
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
if(ASNGN_WITH_LLAMA)
  # The guard shim is C++: it is the only place a C++ exception escaping
  # llama.cpp's C API can be caught before it unwinds into C callers and
  # terminates the process (llama_guard.h has the full rationale).
  enable_language(CXX)
  list(APPEND ASNGN_SOURCES src/llama_guard.cpp)
  if (MSVC)
    # /EHs, not the default /EHsc: the guard exists to catch C++ exceptions
    # arriving through the extern "C" llama API. Under /EHsc MSVC assumes
    # extern "C" callees never throw and elides exactly those catch
    # handlers, turning every such exception into a silent fail-fast.
    set_source_files_properties(src/llama_guard.cpp PROPERTIES COMPILE_OPTIONS "/EHs")
  endif()
endif()

add_library(asngn_obj OBJECT ${ASNGN_SOURCES})
target_include_directories(asngn_obj
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${ASNGN_ASPER_DIR}/include
          ${ASNGN_ASTOOLS_DIR}/include)
target_compile_options(asngn_obj PRIVATE ${ASNGN_C_FLAGS})
target_link_libraries(asngn_obj PUBLIC xcdn asmodel_static)
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
if(WIN32)
  # The DLL's import library is also named asngn.lib and would overwrite
  # asngn_static's archive (last writer wins). Keep it in its own dir.
  set_target_properties(asngn_shared PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/shared-import)
endif()

foreach(tgt asngn_static asngn_shared)
  target_include_directories(${tgt} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(${tgt} PUBLIC xcdn asmodel_static asper_static astools_static)
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
