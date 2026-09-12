cmake_minimum_required(VERSION 3.30)
project(crypto_trader CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(crypto_trader
  src/greeter.cpp
  src/main.cpp
)

# Emits a per-translation-unit *.json timing trace next to each object file,
# for later aggregation with ClangBuildAnalyzer (see decisions/0002).
target_compile_options(crypto_trader PRIVATE -ftime-trace)
