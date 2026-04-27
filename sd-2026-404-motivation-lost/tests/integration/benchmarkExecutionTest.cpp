#include <array>
#include <cstdio>
#include <gtest/gtest.h>
#include <string>

namespace
{
    // Injected at compile time by CMake — points to the built benchmark binary.
    constexpr const char* BENCHMARK_BIN = BENCHMARK_HPC_BIN;

    // A minimal run uses the smallest graph size to keep CI fast.
    // The full suite is invoked by `make benchmark`, not by this test.
    constexpr const char* SMOKE_ENV = "BENCHMARK_SMOKE=1";

    constexpr int OUTPUT_BUFFER_SIZE = 256;

    /// Runs the benchmark binary with popen and returns its exit code.
    /// Discards stdout — we only care that it terminates cleanly.
    int runBenchmarkProcess()
    {
        // Set the smoke env var so the harness can optionally limit scope.
        const std::string command = std::string(SMOKE_ENV) + " " + BENCHMARK_BIN + " 2>&1";
        FILE* pipe = popen(command.c_str(), "r");
        if (pipe == nullptr)
            return -1;

        // Drain output to prevent pipe buffer from blocking the child.
        std::array<char, OUTPUT_BUFFER_SIZE> buffer {};
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
        {
        }

        return pclose(pipe);
    }
} // namespace

TEST(BenchmarkHarness, SmokeTest)
{
    const int exitCode = runBenchmarkProcess();
    EXPECT_EQ(exitCode, 0) << "benchmark_hpc exited with code " << exitCode << " — possible segfault or runtime error";
}
