#include "hpc/graphAnalyzer.hpp"
#include <algorithm>
#include <atomic>
#include <limits>
#include <numeric>
#include <omp.h>
#include <queue>
#include <stdexcept>
#include <vector>

namespace eop::hpc
{
    namespace
    {
        constexpr uint32_t INF = std::numeric_limits<uint32_t>::max();
        constexpr uint32_t PARALLEL_DEGREE_THRESHOLD = 1024;
        constexpr int PARALLEL_FRONTIER_THRESHOLD = 1024;
    } // namespace

    DijkstraResult GraphAnalyzer::dijkstra(const CsrGraph& graph, NodeId source)
    {
        const NodeId nodeCount = graph.getNodeCount();
        if (source >= nodeCount)
            throw std::out_of_range("Dijkstra source node exceeds valid node count");

        using State = std::pair<uint32_t, NodeId>;

        std::vector<std::atomic<uint32_t>> distAtomic(nodeCount);
        for (NodeId i = 0; i < nodeCount; ++i) distAtomic[i].store(INF, std::memory_order_relaxed);
        distAtomic[source].store(0, std::memory_order_relaxed);

        std::priority_queue<State, std::vector<State>, std::greater<>> globalPQ;
        globalPQ.emplace(0u, source);

        const int threadCount = omp_get_max_threads();
        std::vector<std::vector<State>> localHeaps(static_cast<size_t>(threadCount));

        while (!globalPQ.empty())
        {
            const uint32_t currentDist = globalPQ.top().first;
            const NodeId u = globalPQ.top().second;
            globalPQ.pop();

            if (currentDist != distAtomic[u].load(std::memory_order_relaxed))
                continue;

            const uint32_t degree = graph.getNodeDegree(u);
            const NodeId* neighbors = graph.getNeighbors(u);
            const Weight* weights = graph.getWeights(u);

            if (degree == 0 || neighbors == nullptr || weights == nullptr)
                continue;

            for (auto& h : localHeaps) h.clear();

            if (degree < PARALLEL_DEGREE_THRESHOLD)
            {
                for (uint32_t i = 0; i < degree; ++i)
                {
                    const NodeId v = neighbors[i];
                    const Weight w = weights[i];
                    const uint64_t candidate64 = static_cast<uint64_t>(currentDist) + static_cast<uint64_t>(w);
                    const uint32_t candidate = (candidate64 > INF) ? INF : static_cast<uint32_t>(candidate64);
                    if (candidate == INF)
                        continue;

                    uint32_t observed = distAtomic[v].load(std::memory_order_relaxed);
                    while (candidate < observed)
                    {
                        if (distAtomic[v].compare_exchange_weak(
                                observed, candidate, std::memory_order_relaxed, std::memory_order_relaxed))
                        {
                            localHeaps[0].emplace_back(candidate, v);
                            break;
                        }
                    }
                }
            }
            else
            {
#pragma omp parallel for schedule(static) default(none)                                                                \
    shared(degree, neighbors, weights, distAtomic, localHeaps, currentDist)
                for (int i = 0; i < static_cast<int>(degree); ++i)
                {
                    const NodeId v = neighbors[i];
                    const Weight w = weights[i];
                    const uint64_t candidate64 = static_cast<uint64_t>(currentDist) + static_cast<uint64_t>(w);
                    const uint32_t candidate = (candidate64 > INF) ? INF : static_cast<uint32_t>(candidate64);
                    if (candidate == INF)
                        continue;

                    const int tid = omp_get_thread_num();
                    uint32_t observed = distAtomic[v].load(std::memory_order_relaxed);
                    while (candidate < observed)
                    {
                        if (distAtomic[v].compare_exchange_weak(
                                observed, candidate, std::memory_order_relaxed, std::memory_order_relaxed))
                        {
                            localHeaps[static_cast<size_t>(tid)].emplace_back(candidate, v);
                            break;
                        }
                    }
                }
            }

            for (const auto& heap : localHeaps)
                for (const auto& entry : heap) globalPQ.push(entry);
        }

        std::vector<uint32_t> distances(nodeCount);
        for (NodeId i = 0; i < nodeCount; ++i) distances[i] = distAtomic[i].load(std::memory_order_relaxed);

        return DijkstraResult {source, std::move(distances)};
    }

    ComponentsResult GraphAnalyzer::connectedComponents(const CsrGraph& graph)
    {
        const NodeId nodeCount = graph.getNodeCount();
        if (nodeCount == 0)
            return {0, {}};

        std::vector<uint32_t> componentId(nodeCount, 0);

        std::vector<std::atomic<uint8_t>> visited(nodeCount);
        for (NodeId i = 0; i < nodeCount; ++i) visited[i].store(0, std::memory_order_relaxed);

        std::vector<NodeId> frontier;
        frontier.reserve(nodeCount);
        std::vector<NodeId> nextFrontier;
        nextFrontier.reserve(nodeCount);

        const int threadCount = omp_get_max_threads();
        std::vector<std::vector<NodeId>> localNext(static_cast<size_t>(threadCount));
        for (auto& ln : localNext) ln.reserve((nodeCount / static_cast<NodeId>(threadCount)) + 1024u);

        uint32_t currentComponent = 0;

        for (NodeId seed = 0; seed < nodeCount; ++seed)
        {
            uint8_t expected = 0;
            if (!visited[seed].compare_exchange_strong(expected, 1, std::memory_order_relaxed))
                continue;

            currentComponent++;
            componentId[seed] = currentComponent;

            frontier.clear();
            frontier.push_back(seed);

            while (!frontier.empty())
            {
                for (auto& ln : localNext) ln.clear();
                const int frontierSize = static_cast<int>(frontier.size());

                if (frontierSize < PARALLEL_FRONTIER_THRESHOLD)
                {
                    for (int f = 0; f < frontierSize; ++f)
                    {
                        const NodeId u = frontier[static_cast<size_t>(f)];
                        const uint32_t degree = graph.getNodeDegree(u);
                        const NodeId* neighbors = graph.getNeighbors(u);

                        for (uint32_t i = 0; i < degree; ++i)
                        {
                            const NodeId v = neighbors[i];
                            // Test-and-Test-and-Set: Unlocked fast read first!
                            if (visited[v].load(std::memory_order_relaxed) == 0)
                            {
                                uint8_t unvis = 0;
                                if (visited[v].compare_exchange_strong(unvis, 1, std::memory_order_relaxed))
                                {
                                    componentId[v] = currentComponent;
                                    localNext[0].push_back(v);
                                }
                            }
                        }
                    }
                }
                else
                {
                    // Guided schedule naturally balances massive hubs with small nodes without lock overhead
#pragma omp parallel for schedule(guided) default(none)                                                                \
    shared(frontierSize, frontier, graph, visited, componentId, currentComponent, localNext)
                    for (int f = 0; f < frontierSize; ++f)
                    {
                        const NodeId u = frontier[static_cast<size_t>(f)];
                        const uint32_t degree = graph.getNodeDegree(u);
                        const NodeId* neighbors = graph.getNeighbors(u);
                        const int tid = omp_get_thread_num();

                        for (uint32_t i = 0; i < degree; ++i)
                        {
                            const NodeId v = neighbors[i];
                            if (visited[v].load(std::memory_order_relaxed) == 0)
                            {
                                uint8_t unvis = 0;
                                if (visited[v].compare_exchange_strong(unvis, 1, std::memory_order_relaxed))
                                {
                                    componentId[v] = currentComponent;
                                    localNext[static_cast<size_t>(tid)].push_back(v);
                                }
                            }
                        }
                    }
                }

                nextFrontier.clear();
                for (const auto& ln : localNext)
                {
                    nextFrontier.insert(nextFrontier.end(), ln.begin(), ln.end());
                }
                frontier.swap(nextFrontier);
            }
        }

        return {currentComponent, std::move(componentId)};
    }

    CentralityResult GraphAnalyzer::degreeCentrality(const CsrGraph& graph)
    {
        const NodeId nodeCount = graph.getNodeCount();
        std::vector<double> centralityScores(nodeCount, 0.0);
        if (nodeCount <= 1)
            return CentralityResult {std::move(centralityScores)};

        const double denominator = static_cast<double>(nodeCount - 1);

#pragma omp parallel for schedule(static) default(none) shared(nodeCount, graph, centralityScores, denominator)
        for (int i = 0; i < static_cast<int>(nodeCount); ++i)
        {
            const uint32_t degree = graph.getNodeDegree(static_cast<NodeId>(i));
            centralityScores[static_cast<size_t>(i)] = static_cast<double>(degree) / denominator;
        }

        return CentralityResult {std::move(centralityScores)};
    }
} // namespace eop::hpc
