cmake_minimum_required(VERSION 3.30)
project(crypto_trader CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Export compilation database (single file compilation and VSCode integration)

# Set to 1 to use the sanitizers.
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
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
      -Wno-unknown-pragmas
      -Wno-comment
      -Wno-misleading-indentation
      -Wno-deprecated-declarations
      -Wno-array-bounds
      -Wno-redundant-move
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
    # NOTE: semicolons, not commas -- inside a genex's true-branch, commas are
    # literal characters (one opaque string), not the CMake list separator.
    $<$<CONFIG:Release>:-O3;-fomit-frame-pointer;-DNDEBUG;-DQT_NO_DEBUG>
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

# Wraps add_executable() + the standard project_options/project_warnings
# linkage so future targets can't forget to link one -- see decisions/0002.
function(add_project_executable name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE project_options project_warnings)
  if(ENABLE_GPERFTOOLS)
    target_link_libraries(${name} PRIVATE gperftools_profiler)
  endif()
endfunction()

add_project_executable(crypto_trader
  src/greeter.cpp
  src/main.cpp
)
