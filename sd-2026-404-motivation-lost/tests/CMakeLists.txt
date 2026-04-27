find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

#=============================================================================
# Server unit tests
#=============================================================================
add_executable(unit_tests
    unit/exodusBannerTest.cpp
    unit/serverConfigTest.cpp
    unit/socketAcceptorTest.cpp
    unit/sessionManagerTest.cpp
    unit/messageSerializerTest.cpp
    unit/testMessageSerializerAnalyze.cpp
    unit/connectionWorkerTest.cpp
    unit/loggerTest.cpp
    unit/nodeRegistryTest.cpp
    unit/workQueueTest.cpp
    unit/threadPoolTest.cpp
    unit/heartbeatHandlerTest.cpp
    unit/heartbeatMonitorTest.cpp
    unit/analyzeHandlerTest.cpp
    unit/otelTracePropagationTest.cpp
    unit/csrGraphTest.cpp
    unit/graphParserTest.cpp
    unit/graphAlgorithmsTest.cpp
    unit/analysisResultTest.cpp
    unit/hpcHandlerTest.cpp
    unit/engineTopologiesTest.cpp
    unit/engineConfigTest.cpp
    unit/parallelDijkstraTest.cpp
    unit/parallelComponentsTest.cpp
)

target_include_directories(unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(unit_tests PRIVATE exodus_lib GTest::gtest GTest::gtest_main)

#=============================================================================
# Integration tests
#=============================================================================
add_executable(integration_tests
        integration/handshakeIntegrationTest.cpp
        integration/sigKillResilenceTest.cpp
        integration/infraestructureLoadTest.cpp
        integration/latencyBenchmarkTest.cpp
        integration/heartbeatLifecycleTest.cpp
        integration/eopClientLifecycleTest.cpp
        integration/traceGenerationTest.cpp
        integration/hpcEngineE2ETest.cpp
        integration/hpcSla10kTest.cpp
        integration/testHpcIntegration.cpp
)

target_include_directories(integration_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(integration_tests PRIVATE exodus_lib eop_client_static GTest::gtest GTest::gtest_main)

add_dependencies(integration_tests connect_basic_example hpc_engine)

# Inyecta las rutas absolutas al C++
target_compile_definitions(integration_tests PRIVATE
        CLIENT_BIN_PATH=\"$<TARGET_FILE:connect_basic_example>\"
        HPC_ENGINE_BIN=\"$<TARGET_FILE:hpc_engine>\"
        PROJECT_ROOT_DIR=\"${PROJECT_SOURCE_DIR}\"
)

#=============================================================================
# HPC Benchmark harness
# Run via: make benchmark
#=============================================================================
add_executable(benchmark_hpc
        ${PROJECT_SOURCE_DIR}/benchmarks/benchmarkHpc.cpp
)
target_include_directories(benchmark_hpc PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(benchmark_hpc PRIVATE exodus_lib OpenMP::OpenMP_CXX)

#=============================================================================
# Benchmark smoke integration test
#=============================================================================
if(TARGET benchmark_hpc)
    add_executable(benchmark_integration_test
            integration/benchmarkExecutionTest.cpp
    )
    target_include_directories(benchmark_integration_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(benchmark_integration_test PRIVATE GTest::gtest GTest::gtest_main)
    target_compile_definitions(benchmark_integration_test PRIVATE
            BENCHMARK_HPC_BIN=\"$<TARGET_FILE:benchmark_hpc>\"
    )
    add_dependencies(benchmark_integration_test benchmark_hpc)
    gtest_discover_tests(benchmark_integration_test PROPERTIES LABELS "benchmark")
endif()

#=============================================================================
# Client connect tests
#=============================================================================
add_executable(eop_client_connect_test unit/eopClientConnectTest.cpp)

target_link_libraries(eop_client_connect_test PRIVATE eop_client_static GTest::gtest GTest::gtest_main)

#=============================================================================
# Envelope tests
#=============================================================================
add_executable(eop_envelope_test unit/eopEnvelopeTest.cpp)

target_link_libraries(eop_envelope_test PRIVATE eop_client_static GTest::gtest GTest::gtest_main)
target_include_directories(eop_envelope_test PRIVATE ${PROJECT_SOURCE_DIR}/client/src)

#=============================================================================
# Client syscall mock tests
#=============================================================================
add_executable(eop_client_syscall_fail_test unit/eopClientSyscallFailTest.cpp)

target_link_libraries(eop_client_syscall_fail_test PRIVATE eop_client_static GTest::gtest GTest::gtest_main)
target_link_options(eop_client_syscall_fail_test PRIVATE
    -Wl,--wrap=malloc
    -Wl,--wrap=socket
    -Wl,--wrap=connect
    -Wl,--wrap=poll
)

#=============================================================================
# eop_send_command tests
#=============================================================================
add_executable(eop_send_command_test unit/eopSendCommandTest.cpp)

target_link_libraries(eop_send_command_test PRIVATE eop_client_static GTest::gtest GTest::gtest_main)
target_include_directories(eop_send_command_test PRIVATE ${PROJECT_SOURCE_DIR}/client/src)

#=============================================================================
# eop_send_command syscall mock tests
#=============================================================================
add_executable(eop_send_command_syscall_test unit/eopSendCommandSyscallTest.cpp)

target_link_libraries(eop_send_command_syscall_test PRIVATE eop_client_static GTest::gtest GTest::gtest_main)
target_link_options(eop_send_command_syscall_test PRIVATE
    -Wl,--wrap=malloc
    -Wl,--wrap=send
    -Wl,--wrap=recv
    -Wl,--wrap=poll
)

#=============================================================================
# Thread-safety tests
#=============================================================================
add_executable(eop_thread_safety_test unit/eopThreadSafetyTest.cpp)

target_link_libraries(eop_thread_safety_test PRIVATE eop_client_static GTest::gtest GTest::gtest_main)
target_include_directories(eop_thread_safety_test PRIVATE ${PROJECT_SOURCE_DIR}/client/src)

#=============================================================================
# eop_send_command AC tests (comprehensive fixture)
#=============================================================================
add_executable(eop_client_command_test unit/eopClientCommandTest.cpp)

target_link_libraries(eop_client_command_test PRIVATE eop_client_static GTest::gtest GTest::gtest_main)
target_include_directories(eop_client_command_test PRIVATE ${PROJECT_SOURCE_DIR}/client/src)

#=============================================================================
# eop_disconnect stress tests (sequential 1000 cycles + fd leak detection)
#=============================================================================
add_executable(eop_client_disconnect_test unit/eopClientDisconnectTest.cpp)

target_link_libraries(eop_client_disconnect_test PRIVATE eop_client_static GTest::gtest GTest::gtest_main pthread)
target_include_directories(eop_client_disconnect_test PRIVATE ${PROJECT_SOURCE_DIR}/client/src)

#=============================================================================
# Client lifecycle integration test (Task 5 - US-107)
#=============================================================================
add_executable(eop_client_lifecycle_test integration/eopClientLifecycleTest.cpp)

target_include_directories(eop_client_lifecycle_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(eop_client_lifecycle_test PRIVATE exodus_lib eop_client_static GTest::gtest GTest::gtest_main pthread)

#=============================================================================
# Pod restart recovery integration test (Task 4 - US-109)
#=============================================================================
add_executable(eop_pod_restart_recovery_test integration/podRestartRecoveryTest.cpp)

target_include_directories(eop_pod_restart_recovery_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(eop_pod_restart_recovery_test PRIVATE exodus_lib eop_client_static GTest::gtest GTest::gtest_main pthread)

#=============================================================================
# eop_reconnect_probe — CLI helper for pod_restart_recovery_flow.sh (AC6/AC7)
#
# Pure C executable that exercises eop_reconnect() against a live server.
# Not a GTest binary — invoked directly by the shell integration test.
#=============================================================================
add_executable(eop_reconnect_probe integration/eop_reconnect_probe.c)

set_target_properties(eop_reconnect_probe PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)
target_link_libraries(eop_reconnect_probe PRIVATE eop_client_static)

#=============================================================================
# Register tests with CTest
#=============================================================================
gtest_discover_tests(unit_tests)
gtest_discover_tests(integration_tests)
gtest_discover_tests(eop_client_lifecycle_test PROPERTIES LABELS "integration")
gtest_discover_tests(eop_pod_restart_recovery_test PROPERTIES LABELS "integration")
gtest_discover_tests(eop_client_connect_test)
gtest_discover_tests(eop_envelope_test)
gtest_discover_tests(eop_client_syscall_fail_test)
gtest_discover_tests(eop_send_command_test)
gtest_discover_tests(eop_send_command_syscall_test)
gtest_discover_tests(eop_thread_safety_test)
gtest_discover_tests(eop_client_command_test)
gtest_discover_tests(eop_client_disconnect_test)

#=============================================================================
# Coverage
#=============================================================================
if(EXODUS_ENABLE_COVERAGE)
    foreach(target
        unit_tests
        integration_tests
        eop_client_connect_test
        eop_client_syscall_fail_test
        eop_send_command_test
        eop_send_command_syscall_test
        eop_thread_safety_test
        eop_client_command_test
        eop_client_disconnect_test
        eop_client_lifecycle_test
        eop_pod_restart_recovery_test
        eop_reconnect_probe
        benchmark_hpc
        benchmark_integration_test
    )
        target_compile_options(${target} PRIVATE -O0 -g --coverage)
        target_link_options(${target} PRIVATE --coverage)
    endforeach()

    target_compile_options(exodus_lib PRIVATE -O0 -g --coverage)
endif()
