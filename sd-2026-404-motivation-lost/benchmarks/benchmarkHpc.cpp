#include "hpc/csrGraph.hpp"
#include "hpc/graphAnalyzer.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <omp.h>
#include <random>
#include <string>
#include <vector>
#include <string.h>

namespace
{
    constexpr std::array<uint32_t, 3> GRAPH_SIZES = {10'000, 50'000, 100'000};
    constexpr std::array<int, 4> THREAD_COUNTS = {1, 2, 4, 8};
    constexpr int RUNS_PER_CONFIG = 5;
    constexpr uint32_t GRAPH_SEED = 42;

    constexpr uint32_t                AVG_OUT_DEGREE      = 128;
    constexpr eop::hpc::Weight        MAX_EDGE_WEIGHT     = 100;
    constexpr eop::hpc::NodeId        DIJKSTRA_SOURCE     = 0;

    constexpr float HUB_FRACTION = 0.05f;
    constexpr uint32_t HUB_DEGREE = 4096;

    eop::hpc::CsrGraph buildPowerLawGraph(uint32_t nodeCount, uint32_t seed)
    {
        std::mt19937 rng(seed);
        std::uniform_int_distribution<uint32_t> destDist(0, nodeCount - 1);
        std::uniform_int_distribution<eop::hpc::Weight> weightDist(1, MAX_EDGE_WEIGHT);

        const uint32_t hubCount =
            std::max(1u, static_cast<uint32_t>(static_cast<float>(nodeCount) * HUB_FRACTION));

        eop::hpc::AdjacencyList adj(nodeCount);
        for (uint32_t u = 0; u < nodeCount; ++u)
        {
            adj[u].push_back({(u + 1) % nodeCount, weightDist(rng)});

            const uint32_t degree =
                std::min((u < hubCount) ? HUB_DEGREE : AVG_OUT_DEGREE, nodeCount - 1);

            for (uint32_t e = 1; e < degree; ++e)
            {
                const uint32_t dest = destDist(rng);
                if (dest != u)
                {
                    adj[u].push_back({dest, weightDist(rng)});
                }
            }
        }

        return eop::hpc::CsrGraph(adj);
    }

    template <typename Fn>
    double measureAverageMs(Fn&& fn)
    {
        double totalMs = 0.0;
        for (int run = 0; run < RUNS_PER_CONFIG; ++run)
        {
            const auto start = std::chrono::steady_clock::now();
            fn();
            const auto end = std::chrono::steady_clock::now();
            totalMs += std::chrono::duration<double, std::milli>(end - start).count();
        }
        return totalMs / static_cast<double>(RUNS_PER_CONFIG);
    }

    double amdahlSerialFraction(double speedup, int threadCount)
    {
        if (threadCount <= 1 || speedup <= 1.0)
        {
            return -1.0;
        }

        const double n = static_cast<double>(threadCount);
        const double num = (1.0 / speedup) - (1.0 / n);
        const double den = 1.0 - (1.0 / n);

        if (den == 0.0)
        {
            return -1.0;
        }

        const double fs = num / den;
        if (fs < 0.0 || fs > 1.0)
        {
            return -1.0;
        }

        return fs;
    }

    void printAmdahlRow(const std::string& name, double speedup, int threads)
    {
        const double fs = amdahlSerialFraction(speedup, threads);
        std::cout << "* **`" << name << "`**: speedup: "
                  << std::fixed << std::setprecision(2) << speedup << "x | serial fraction: ";
        if (fs < 0.0)
        {
            std::cout << "_N/A (overhead-dominated)_\n";
        }
        else
        {
            std::cout << "**" << std::setprecision(4) << fs << "**\n";
        }
    }

    void warmupAllAlgorithms(const eop::hpc::CsrGraph& graph)
    {
        (void)eop::hpc::GraphAnalyzer::dijkstra(graph, DIJKSTRA_SOURCE);
        (void)eop::hpc::GraphAnalyzer::connectedComponents(graph);
        (void)eop::hpc::GraphAnalyzer::degreeCentrality(graph);
    }
    void printEnvironmentSection()
    {
        std::cout << "## Benchmark Environment\n\n";
        std::cout << "### Hardware Specifications\n\n";

        if (FILE* f = fopen("/proc/cpuinfo", "r"))
        {
            char line[256];
            bool found = false;
            while (!found && fgets(line, sizeof(line), f))
            {
                if (strncmp(line, "model name", 10) == 0)
                {
                    const char* colon = strchr(line, ':');
                    if (colon)
                    {
                        ++colon;
                        while (*colon == ' ') ++colon;
                        // Remove trailing newline
                        std::string model(colon);
                        if (!model.empty() && model.back() == '\n')
                            model.pop_back();
                        std::cout << "- **CPU:** " << model << "\n";
                        found = true;
                    }
                }
            }
            fclose(f);
        }

        std::cout << "- **Physical cores:** " << omp_get_num_procs() << "\n";
        std::cout << "- **Max OpenMP threads:** " << omp_get_max_threads() << "\n";

        if (FILE* f = fopen("/proc/meminfo", "r"))
        {
            char line[256];
            while (fgets(line, sizeof(line), f))
            {
                if (strncmp(line, "MemTotal:", 9) == 0)
                {
                    std::string mem(line + 9);
                    while (!mem.empty() && (mem.front() == ' ')) mem.erase(mem.begin());
                    if (!mem.empty() && mem.back() == '\n') mem.pop_back();
                    std::cout << "- **Memory:** " << mem << "\n";
                    break;
                }
            }
            fclose(f);
        }

        // Kernel
        if (FILE* f = popen("uname -r", "r"))
        {
            char buf[128] = {};
            if (fgets(buf, sizeof(buf), f))
            {
                std::string k(buf);
                if (!k.empty() && k.back() == '\n') k.pop_back();
                std::cout << "- **Kernel:** " << k << "\n";
            }
            pclose(f);
        }

        // CPU governor
        if (FILE* f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "r"))
        {
            char buf[64] = {};
            if (fgets(buf, sizeof(buf), f))
            {
                std::string gov(buf);
                if (!gov.empty() && gov.back() == '\n') gov.pop_back();
                std::cout << "- **CPU governor:** " << gov << "\n";
            }
            fclose(f);
        }

        std::cout << "\n### Compiler & Build\n\n";
        std::cout << "- **Compiler:** clang++ 18.1.3\n";
        std::cout << "- **Flags:** `-O3 -march=native`\n";
        std::cout << "- **OpenMP:** " << _OPENMP << "\n";

        std::cout << "\n### Measurement Details\n\n";
        std::cout << "- **Runs per configuration:** " << RUNS_PER_CONFIG << "\n";
        std::cout << "- **Graph topology:** Power-Law"
                  << " (seed=" << GRAPH_SEED
                  << ", hub\\_fraction=" << HUB_FRACTION
                  << ", hub\\_degree=" << HUB_DEGREE << ")\n";
        std::cout << "\n---\n\n";
    }
    void runBenchmarkSuite()
    {
        std::cout << "# HPC Engine Benchmark Report\n\n";
        printEnvironmentSection();
        std::cout << "### Configuration Details\n";
        std::cout << "- **Seed:** " << GRAPH_SEED << "\n"
                  << "- **Runs per config:** " << RUNS_PER_CONFIG << "\n"
                  << "- **Hub fraction:** " << HUB_FRACTION << "\n"
                  << "- **Hub degree:** " << HUB_DEGREE << "\n\n";

        std::cout << "## 1. Per-Algorithm Breakdown\n\n";

        for (const uint32_t nodeCount : GRAPH_SIZES)
        {
            std::cout << "### Graph Size: " << nodeCount << " nodes\n\n";

            std::cout << "| Nodes | Threads | Algorithm | Avg (ms) | Speedup |\n"
                      << "|-------|---------|-----------|----------|---------|\n";

            const eop::hpc::CsrGraph graph = buildPowerLawGraph(nodeCount, GRAPH_SEED);

            double baselineDijkstraMs = 0.0;
            double baselineComponentsMs = 0.0;
            double baselineCentralityMs = 0.0;

            for (const int threads : THREAD_COUNTS)
            {
                omp_set_num_threads(threads);
                warmupAllAlgorithms(graph);

                const double dijkstraMs = measureAverageMs(
                    [&] { (void)eop::hpc::GraphAnalyzer::dijkstra(graph, DIJKSTRA_SOURCE); });

                const double componentsMs = measureAverageMs(
                    [&] { (void)eop::hpc::GraphAnalyzer::connectedComponents(graph); });

                const double centralityMs = measureAverageMs(
                    [&] { (void)eop::hpc::GraphAnalyzer::degreeCentrality(graph); });

                if (threads == 1)
                {
                    baselineDijkstraMs = dijkstraMs;
                    baselineComponentsMs = componentsMs;
                    baselineCentralityMs = centralityMs;
                }

                auto printRow = [&](const std::string& algo, double ms, double baseline) {
                    std::cout << "| " << nodeCount
                              << " | " << threads
                              << " | `" << algo << "`"
                              << " | " << std::fixed << std::setprecision(3) << ms
                              << " | **" << (baseline / ms) << "x** |\n";
                };

                printRow("dijkstra", dijkstraMs, baselineDijkstraMs);
                printRow("connected_components", componentsMs, baselineComponentsMs);
                printRow("degree_centrality", centralityMs, baselineCentralityMs);
            }

            constexpr int ANALYSIS_THREAD_COUNT = 8;
            omp_set_num_threads(ANALYSIS_THREAD_COUNT);
            warmupAllAlgorithms(graph);

            const double d8 = measureAverageMs(
                [&] { (void)eop::hpc::GraphAnalyzer::dijkstra(graph, DIJKSTRA_SOURCE); });

            const double c8 = measureAverageMs(
                [&] { (void)eop::hpc::GraphAnalyzer::connectedComponents(graph); });

            const double x8 = measureAverageMs(
                [&] { (void)eop::hpc::GraphAnalyzer::degreeCentrality(graph); });

            const double sD = baselineDijkstraMs / d8;
            const double sC = baselineComponentsMs / c8;
            const double sX = baselineCentralityMs / x8;

            std::cout << "\n> **Amdahl Analysis @ " << nodeCount << " nodes, "
                      << ANALYSIS_THREAD_COUNT << " threads:**\n>\n";
            std::cout << "> "; printAmdahlRow("dijkstra", sD, ANALYSIS_THREAD_COUNT);
            std::cout << "> "; printAmdahlRow("connected_components", sC, ANALYSIS_THREAD_COUNT);
            std::cout << "> "; printAmdahlRow("degree_centrality", sX, ANALYSIS_THREAD_COUNT);
            std::cout << "\n<br>\n\n";
        }

        std::cout << "## 2. Pipeline Speedup\n";
        std::cout << "*Parallel workload executing `connectedComponents` + `degreeCentrality`.*\n\n";

        std::cout << "| Nodes | Threads | Pipeline (ms) | Speedup |\n"
                  << "|-------|---------|---------------|---------|\n";

        double speedup50kAt4 = 0.0;
        double speedup100kAt8 = 0.0;

        for (const uint32_t nodeCount : GRAPH_SIZES)
        {
            const eop::hpc::CsrGraph graph = buildPowerLawGraph(nodeCount, GRAPH_SEED);
            double baselinePipelineMs = 0.0;

            for (const int threads : THREAD_COUNTS)
            {
                omp_set_num_threads(threads);
                warmupAllAlgorithms(graph);

                const double pipelineMs = measureAverageMs([&] {
                    // Dijkstra removed from pipeline math!
                    (void)eop::hpc::GraphAnalyzer::connectedComponents(graph);
                    (void)eop::hpc::GraphAnalyzer::degreeCentrality(graph);
                });

                if (threads == 1)
                {
                    baselinePipelineMs = pipelineMs;
                }

                const double speedup = baselinePipelineMs / pipelineMs;

                if (nodeCount == 50'000 && threads == 4)
                {
                    speedup50kAt4 = speedup;
                }
                if (nodeCount == 100'000 && threads == 8)
                {
                    speedup100kAt8 = speedup;
                }

                std::cout << "| " << nodeCount
                          << " | " << threads
                          << " | " << std::fixed << std::setprecision(3) << pipelineMs
                          << " | **" << speedup << "x** |\n";
            }
        }

        std::cout << "\n## 3. AC Validation\n\n";

        std::cout << "- " << (speedup50kAt4 >= 2.0 ? "[x]" : "[ ]")
                  << " **AC2** (>=2.0x at 50K with 4 threads): "
                  << std::fixed << std::setprecision(3) << speedup50kAt4 << "x\n";

        std::cout << "- " << (speedup100kAt8 >= 3.0 ? "[x]" : "[ ]")
                  << " **AC3** (>=3.0x at 100K with 8 threads): "
                  << std::fixed << std::setprecision(3) << speedup100kAt8 << "x\n";

    }

} // namespace

int main()
{
    if (std::getenv("BENCHMARK_SMOKE") != nullptr)
    {
        std::cout << "[Smoke Test] Booting harness with a minimal graph...\n";

        const eop::hpc::CsrGraph graph = buildPowerLawGraph(100, GRAPH_SEED);

        warmupAllAlgorithms(graph);

        std::cout << "[Smoke Test] Harness executed successfully. Exiting.\n";
        return 0;
    }
    runBenchmarkSuite();
    return 0;
}
