cmake_minimum_required(VERSION 3.16)
project(cachescope LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(CACHESCOPE_BUILD_TESTS "Build the GoogleTest unit tests" ON)
option(CACHESCOPE_WERROR "Treat compiler warnings as errors" OFF)
option(CACHESCOPE_NATIVE "Build with -march=native (not portable)" OFF)

# ---------------------------------------------------------------- core library

add_library(cachescope_core
  src/types.cpp
  src/json.cpp
  src/config.cpp
  src/counters.cpp
  src/replacement.cpp
  src/cache.cpp
  src/prefetcher.cpp
  src/dram.cpp
  src/hierarchy.cpp
  src/predictor.cpp
  src/trace.cpp
  src/telemetry.cpp
  src/engine.cpp
)

target_include_directories(cachescope_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(MSVC)
  target_compile_options(cachescope_core PRIVATE /W4 /permissive-)
  # The simulator is single-threaded and uses no exceptions across DLL
  # boundaries; keep the default model but silence the secure-CRT noise around
  # fopen, which is used deliberately for buffered trace reading.
  target_compile_definitions(cachescope_core PUBLIC _CRT_SECURE_NO_WARNINGS)
  if(CACHESCOPE_WERROR)
    target_compile_options(cachescope_core PRIVATE /WX)
  endif()
else()
  target_compile_options(cachescope_core PRIVATE -Wall -Wextra -Wpedantic)
  if(CACHESCOPE_WERROR)
    target_compile_options(cachescope_core PRIVATE -Werror)
  endif()
  if(CACHESCOPE_NATIVE)
    target_compile_options(cachescope_core PRIVATE -march=native)
  endif()
endif()

# ---------------------------------------------------------------- executable

add_executable(cachescope src/main.cpp)
target_link_libraries(cachescope PRIVATE cachescope_core)

if(MSVC)
  target_compile_definitions(cachescope PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------- tests

if(CACHESCOPE_BUILD_TESTS)
  # Prefer a GoogleTest already on the system; fall back to fetching it. The
  # fallback needs network access at configure time, so CI caches it.
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG v1.15.2
      GIT_SHALLOW TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------- summary

message(STATUS "cachescope ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  tests      : ${CACHESCOPE_BUILD_TESTS}")
