cmake_minimum_required(VERSION 3.28)
project(exodus-eop-v LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
enable_testing()

# =============================================================================
# Opciones
# =============================================================================
option(EXODUS_BUILD_TESTS      "Build unit tests"                    ON)
option(EXODUS_BUILD_BENCHMARKS "Build benchmarks"                    OFF)
option(EXODUS_ENABLE_COVERAGE  "Enable coverage flags (local only)"  OFF)

# =============================================================================
# Flags de compilación
# =============================================================================
add_compile_options(-Wall -Wextra -Werror -Wpedantic)

# =============================================================================
# Biblioteca estática
# =============================================================================
add_library(exodus_lib STATIC
        src/exodusBanner.cpp
        src/serverConfig.cpp
        src/socketAcceptor.cpp
        src/sessionManager.cpp
        src/messageSerializer.cpp
        src/nodeRegistry.cpp
        src/registerHandler.cpp
        src/connectionWorker.cpp
        src/logger.cpp
        src/otelScope.cpp
        src/queryHandler.cpp
        src/workQueue.cpp
        src/threadPool.cpp
        src/heartbeatHandler.cpp
        src/heartbeatMonitor.cpp
)

target_include_directories(exodus_lib PUBLIC include)

find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(exodus_lib PUBLIC nlohmann_json::nlohmann_json)

if(EXODUS_ENABLE_COVERAGE)
  target_compile_options(exodus_lib PRIVATE -O0 -g --coverage)
  target_link_options(exodus_lib PUBLIC --coverage)
endif()

# =============================================================================
# Ejecutable principal
# =============================================================================
add_executable(exodus_app
        src/app/main.cpp
)

target_include_directories(exodus_app PRIVATE include)
target_link_libraries(exodus_app PRIVATE exodus_lib)

# =============================================================================
# Subdirectories
# =============================================================================
add_subdirectory(client)

if(EXODUS_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# =============================================================================
# Benchmarks (Google Benchmark)
# =============================================================================
if(EXODUS_BUILD_BENCHMARKS)
  find_package(benchmark CONFIG REQUIRED)

  add_executable(benchmarks_run
          benchmarks/bench_exodus_banner.cpp
  )

  target_include_directories(benchmarks_run PRIVATE include)
  target_link_libraries(benchmarks_run PRIVATE
          exodus_lib
          benchmark::benchmark
          benchmark::benchmark_main
  )
endif()
