cmake_minimum_required(VERSION 3.30)
project(crypto_trader CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Export compilation database (single file compilation and VSCode integration)

# Set to 1 to use the sanitizers.
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
# ThreadSanitizer instruments differently than ASan/UBSan and the two runtimes
# cannot coexist in one binary, hence the separate option and the guard below
# rather than folding it into ENABLE_ASAN's block. See decisions/0005.
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
if(ENABLE_TSAN AND ENABLE_ASAN)
  message(FATAL_ERROR "ENABLE_TSAN and ENABLE_ASAN cannot be combined in one binary -- configure separate build directories (see scripts/run_sanitizers.sh)")
endif()
option(USE_CLOCK_MANAGER "Enable clock manager" OFF)
option(ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)
option(ENABLE_TCMALLOC "Use tcmalloc" OFF)
option(ENABLE_GPERFTOOLS "Link gperftools CPU profiler" OFF)

# Get the branch name (returns "HEAD" if in detached HEAD state)
execute_process(
  COMMAND git rev-parse --abbrev-ref HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE GIT_BRANCH
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

# #### Build directory ####
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")


if (DEFINED "CMAKE_CXX_COMPILER_ID")
    message ("Using compiler ${CMAKE_CXX_COMPILER_ID}")
else ()
    error("No compiler found")
endif ()

# set(PCH_ENABLE 0 CACHE STRING "")

# Use llvm-ar and llvm-ranlib for faster static library creation
# llvm-ar is significantly faster than GNU ar for large archives
find_program(LLVM_AR llvm-ar)
find_program(LLVM_RANLIB llvm-ranlib)
if (LLVM_AR AND LLVM_RANLIB)
  set(CMAKE_AR "${LLVM_AR}")
  set(CMAKE_RANLIB "${LLVM_RANLIB}")
else()
    message(WARNING "llvm-ar not found, using slower GNU ar")
endif()

set(current_user $ENV{USER})

message("Running using cmake version ${CMAKE_VERSION}")
message("Setting up mode ${CMAKE_BUILD_TYPE} for branch ${GIT_BRANCH}
        and compiler ${CMAKE_CXX_COMPILER_ID} with C++${CMAKE_CXX_STANDARD}")


add_library(project_sanitizers INTERFACE)
if(ENABLE_ASAN)
    target_compile_options(project_sanitizers INTERFACE
        -fsanitize=address
        -fno-omit-frame-pointer
    )
    target_link_options(project_sanitizers INTERFACE
        -fsanitize=address
    )
endif()
if(ENABLE_UBSAN)
    target_compile_options(project_sanitizers INTERFACE
        -fsanitize=undefined
    )
    target_link_options(project_sanitizers INTERFACE
        -fsanitize=undefined
    )
endif()
if(ENABLE_TSAN)
    target_compile_options(project_sanitizers INTERFACE
        -fsanitize=thread
    )
    target_link_options(project_sanitizers INTERFACE
        -fsanitize=thread
    )
endif()

add_library(project_options INTERFACE)
if(USE_CLOCK_MANAGER)
    target_compile_definitions(project_options INTERFACE
        USE_CLOCK_MANAGER
    )
endif()

# LTO only enabled for production (IMPORTANT: needs to be set in both compile and link options)
target_compile_options(project_options INTERFACE
    $<$<CONFIG:production>:-flto=thin>
)
target_link_options(project_options INTERFACE
    -fuse-ld=lld
    # ICF (Identical Code Folding) for optimized builds
    $<$<CONFIG:release,profile,production>:LINKER:--icf=all>
    $<$<CONFIG:production>:-flto=thin>
)

if(ENABLE_COVERAGE)
    target_compile_options(project_options INTERFACE
        --coverage
        -O0
        -g
    )
    target_link_options(project_options INTERFACE
        --coverage
    )
endif()

add_library(project_warnings INTERFACE)
target_compile_options(project_warnings INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
      -Wall
      -Wextra
      -Werror
      -Wshadow
      -Wuninitialized
      -Winit-self
      # Catches silent narrowing/sign-change conversions (dropped bits on a
      # size_t->int truncation, a subtraction wrapping through an unsigned
      # comparison) -- exactly the class of bug that looks fine to a quick
      # read. FetchContent'd dependencies are marked SYSTEM below so this
      # only ever fires on our own code, not third-party headers.
      -Wconversion
      -Wsign-conversion
      -Wno-unknown-pragmas
      -Wno-comment
      -Wno-misleading-indentation
      -Wno-deprecated-declarations
      -Wno-array-bounds
      -Wno-redundant-move
      # clang-tidy's syntax-only analysis pass reports -ftime-trace (see
      # project_options below) as unused since it never runs codegen --
      # without this, -Werror turns that into a hard error on every file.
      -Wno-unused-command-line-argument
      -ftemplate-backtrace-limit=0
    >
)
# GNU specific
target_compile_options(project_warnings INTERFACE
    $<$<CXX_COMPILER_ID:GNU>:
        -Wno-stringop-overflow
        -Wno-restrict
        -Wno-stringop-overread
        -Wno-error=format-security
        -Wno-format
        -Wno-maybe-uninitialized
    >
)

add_library(project_tcmalloc INTERFACE)
if(ENABLE_TCMALLOC)
    target_compile_options(project_tcmalloc INTERFACE
        -fno-builtin-malloc
        -fno-builtin-calloc
        -fno-builtin-realloc
        -fno-builtin-free
    )

    target_link_libraries(project_tcmalloc INTERFACE
        # proper tcmalloc target here
    )
endif()

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")

  target_compile_options(project_options INTERFACE
    # Emits a per-translation-unit *.json timing trace next to each object file for ClangBuildAnalyzer
    -ftime-trace
    -ftemplate-depth=750
    $<$<CONFIG:release,profile,production>:-O3;-fomit-frame-pointer;-DNDEBUG;-DQT_NO_DEBUG>
  )
  target_link_options(project_options INTERFACE
      -fuse-ld=lld
  )
endif ()

#### Google Profiler libraries ####
if(ENABLE_GPERFTOOLS)
  find_library(GPERFTOOLS_PROFILER_LIBRARY NAMES profiler REQUIRED)
  add_library(gperftools_profiler UNKNOWN IMPORTED)
  set_target_properties(gperftools_profiler PROPERTIES IMPORTED_LOCATION "${GPERFTOOLS_PROFILER_LIBRARY}")
endif()

#### Third-party dependencies (decisions/0002, decisions/0004) ####
include(FetchContent)

# IXWebSocket: Kraken WSS transport. USE_TLS must be forced into the cache
# before add_subdirectory runs (via FetchContent_MakeAvailable below) --
# IXWebSocket's own option(USE_TLS ...) call is a no-op once the cache entry
# already exists. Defaults to OpenSSL on Linux once TLS is on.
set(USE_TLS ON CACHE BOOL "" FORCE)
FetchContent_Declare(
  ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG v12.0.1
  SYSTEM
)

# simdjson: Kraken message parsing.
FetchContent_Declare(
  simdjson
  GIT_REPOSITORY https://github.com/simdjson/simdjson.git
  GIT_TAG v4.6.11
  SYSTEM
)

# JSON parsing: order_book_tests' capture-replay fixtures and the
# kraken_journal_dump tool both need it to parse captured wire messages --
# not a production dependency, decisions/0002 defers a real wire/journal
# serialization format.
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  SYSTEM
)
FetchContent_MakeAvailable(ixwebsocket simdjson nlohmann_json)

# GoogleTest: TDD for feed-parsing/journal/order-book logic, per decisions/0002.
option(BUILD_TESTING "Build test targets" ON)
if(BUILD_TESTING)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.18.0
  )
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  include(GoogleTest)
endif()

# Wraps add_executable() + the standard project_options/project_warnings
# linkage so future targets can't forget to link one -- see decisions/0002.
function(add_project_executable name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE project_options project_warnings project_sanitizers)
  if(ENABLE_GPERFTOOLS)
    target_link_libraries(${name} PRIVATE gperftools_profiler)
  endif()
endfunction()

# Same, but for a GTest-based test binary registered with ctest -- see
# decisions/0002.
function(add_project_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE project_options project_warnings project_sanitizers
                                        GTest::gtest_main)
  gtest_discover_tests(${name})
endfunction()

add_project_executable(crypto_trader
  src/greeter.cpp
  src/main.cpp
)

#### Feed handler ####
# OpenSSL and zlib both already arrive transitively through IXWebSocket
# (USE_TLS/USE_ZLIB), but relying on that leakage from a subdirectory scope is
# fragile -- ask for them explicitly for the targets that actually use them.
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

add_library(feed_handler STATIC
  src/feed_handler/capture_session.cpp
  src/feed_handler/journal_format.cpp
  src/feed_handler/journal_reader.cpp
  src/feed_handler/journal_writer.cpp
  src/feed_handler/logging.cpp
  src/feed_handler/message_sink.cpp
  src/feed_handler/deribit/deribit_fix_client.cpp
  src/feed_handler/deribit/deribit_fix_session.cpp
  src/feed_handler/fix/fix_message.cpp
  src/feed_handler/kraken/kraken_rest_client.cpp
  src/feed_handler/kraken/kraken_signing.cpp
  src/feed_handler/kraken/kraken_ws_client.cpp
)
target_include_directories(feed_handler PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(feed_handler
  PUBLIC project_options project_warnings project_sanitizers
  PRIVATE ixwebsocket::ixwebsocket simdjson::simdjson OpenSSL::Crypto ZLIB::ZLIB
)

# The live Kraken level3 capture binary (decisions/0004's first vertical
# slice). Credentials come from the environment; see the file's header comment.
add_project_executable(kraken_feed_handler
  src/feed_handler/kraken/kraken_feed_handler_main.cpp
)
target_link_libraries(kraken_feed_handler PRIVATE feed_handler)

# The live Deribit FIX capture binary: decisions/0004's second exchange
# backend, hand-rolled over a raw POSIX socket. Credentials come from the
# environment; see the file's header comment.
add_project_executable(deribit_feed_handler
  src/feed_handler/deribit/deribit_feed_handler_main.cpp
)
target_link_libraries(deribit_feed_handler PRIVATE feed_handler)

if(BUILD_TESTING)
  # Proves the FetchContent wiring for IXWebSocket/simdjson/GoogleTest
  # actually works end-to-end -- not feed handler behavior.
  add_project_test(dependency_smoke_test
    src/feed_handler/tests/dependency_smoke_test.cpp
  )
  target_link_libraries(dependency_smoke_test PRIVATE ixwebsocket::ixwebsocket simdjson::simdjson)

  add_project_test(feed_handler_tests
    src/feed_handler/tests/capture_session_test.cpp
    src/feed_handler/tests/deribit_fix_client_test.cpp
    src/feed_handler/tests/deribit_fix_session_test.cpp
    src/feed_handler/tests/fix_message_test.cpp
    src/feed_handler/tests/journal_test.cpp
    src/feed_handler/tests/kraken_rest_client_test.cpp
    src/feed_handler/tests/kraken_ws_client_test.cpp
    src/feed_handler/tests/staleness_watchdog_test.cpp
  )
  target_link_libraries(feed_handler_tests PRIVATE feed_handler)
endif()

#### Order book (decisions/0006) ####
# Mostly header-only (templates/concepts), but the Kraken checksum is
# genuinely non-template logic, so this needs a compiled source, same
# reasoning as feed_handler above.
add_library(order_book STATIC
  src/order_book/kraken_checksum.cpp
)
target_include_directories(order_book PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(order_book PUBLIC project_options project_warnings project_sanitizers)

# kraken_l3_wire.h (header-only Kraken level3 message parsing) needs a JSON
# library. Kept as its own target so nlohmann_json is not a dependency of the
# core order_book library, only of what actually parses wire messages.
add_library(order_book_kraken_wire INTERFACE)
target_link_libraries(order_book_kraken_wire INTERFACE order_book nlohmann_json::nlohmann_json)

if(BUILD_TESTING)
  # The replay fixtures are real captured sessions (a Kraken level3 journal and
  # a Deribit book.raw log). They are committed compressed -- ~290 KB against
  # ~3.9 MB raw -- and unpacked into the build tree at configure time, so the
  # tests still read plain files. CMAKE_CONFIGURE_DEPENDS re-runs the extraction
  # whenever the archive changes.
  set(ORDER_BOOK_FIXTURE_ARCHIVE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/order_book/tests/data/capture_fixtures.tar.xz)
  set(ORDER_BOOK_TEST_DATA_DIR ${CMAKE_CURRENT_BINARY_DIR}/order_book_test_data)
  file(ARCHIVE_EXTRACT INPUT ${ORDER_BOOK_FIXTURE_ARCHIVE}
       DESTINATION ${ORDER_BOOK_TEST_DATA_DIR})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ORDER_BOOK_FIXTURE_ARCHIVE})

  add_project_test(order_book_tests
    src/order_book/tests/concepts_test.cpp
    src/order_book/tests/l1_book_test.cpp
    src/order_book/tests/l2_book_test.cpp
    src/order_book/tests/l3_book_test.cpp
    src/order_book/tests/instrument_scale_test.cpp
    src/order_book/tests/kraken_checksum_test.cpp
    src/order_book/tests/kraken_l3_policy_test.cpp
    src/order_book/tests/kraken_l3_wire_test.cpp
    src/order_book/tests/deribit_capture_replay_test.cpp
    src/order_book/tests/kraken_capture_replay_test.cpp
  )
  target_link_libraries(order_book_tests PRIVATE order_book_kraken_wire feed_handler)
  target_compile_definitions(order_book_tests PRIVATE
    TEST_DATA_DIR="${ORDER_BOOK_TEST_DATA_DIR}"
  )
endif()

# Dumps a captured Kraken level3 journal to one JSON frame per message, for
# tools/kraken_journal_viewer.py's step-through visualizer. Reuses the real
# order_book/feed_handler code -- not a second implementation of book logic.
add_project_executable(kraken_journal_dump
  src/order_book/tools/kraken_journal_dump.cpp
)
target_link_libraries(kraken_journal_dump PRIVATE order_book_kraken_wire feed_handler)
