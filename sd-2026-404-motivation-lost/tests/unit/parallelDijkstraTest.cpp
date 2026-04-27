#include "hpc/csrGraph.hpp"
#include "hpc/graphAnalyzer.hpp"
#include <gtest/gtest.h>
#include <limits>
#include <omp.h>
#include <vector>

namespace eop::hpc
{
    namespace
    {
        constexpr uint32_t INF_DISTANCE = std::numeric_limits<uint32_t>::max();
        constexpr int THREAD_COUNT_SINGLE = 1;
        constexpr int THREAD_COUNT_MODERATE = 4;
        constexpr int THREAD_COUNT_HIGH = 8;

        // Graph topology constants — kept named so the adjacency list reads as a spec.
        constexpr NodeId NODE_A = 0;
        constexpr NodeId NODE_B = 1;
        constexpr NodeId NODE_C = 2;
        constexpr NodeId NODE_D = 3;

        constexpr Weight WEIGHT_AB = 1;
        constexpr Weight WEIGHT_AC = 4;
        constexpr Weight WEIGHT_BD = 2;
        constexpr Weight WEIGHT_CD = 1;

        constexpr NodeId RACE_TEST_NODE_COUNT = 500;
        constexpr int RACE_TEST_RUNS = 5;
        constexpr Weight RACE_EDGE_DIRECT = 1;
        constexpr Weight RACE_EDGE_SHORTCUT = 3;

        /**
         * @brief Constructs a weighted directed graph with a known shortest-path solution.
         *
         * Topology:
         *   A --(1)--> B --(2)--> D
         *   |                     ^
         *   +---(4)--> C --(1)----+
         *
         * Shortest paths from A:
         *   d[A]=0, d[B]=1, d[C]=4, d[D]=3  (A->B->D)
         */
        CsrGraph buildKnownGraph()
        {
            AdjacencyList adj(4);
            adj[NODE_A] = {{NODE_B, WEIGHT_AB}, {NODE_C, WEIGHT_AC}};
            adj[NODE_B] = {{NODE_D, WEIGHT_BD}};
            adj[NODE_C] = {{NODE_D, WEIGHT_CD}};
            return CsrGraph(adj);
        }

        /// RAII guard that restores the OpenMP thread count on scope exit,
        /// even if the guarded call throws.
        struct ThreadCountGuard
        {
            const int saved;
            explicit ThreadCountGuard(int n)
                : saved(omp_get_max_threads())
            {
                omp_set_num_threads(n);
            }
            ~ThreadCountGuard()
            {
                omp_set_num_threads(saved);
            }
        };

        DijkstraResult runSerial(const CsrGraph& graph, NodeId source)
        {
            ThreadCountGuard guard(THREAD_COUNT_SINGLE);
            return GraphAnalyzer::dijkstra(graph, source);
        }
    } // namespace

    TEST(ParallelDijkstra, MatchesKnownDistances)
    {
        const CsrGraph graph = buildKnownGraph();
        ThreadCountGuard guard(THREAD_COUNT_MODERATE);
        const DijkstraResult result = GraphAnalyzer::dijkstra(graph, NODE_A);

        ASSERT_EQ(result.distances.size(), 4u);
        EXPECT_EQ(result.distances[NODE_A], 0u);
        EXPECT_EQ(result.distances[NODE_B], WEIGHT_AB);
        EXPECT_EQ(result.distances[NODE_C], WEIGHT_AC);
        EXPECT_EQ(result.distances[NODE_D], WEIGHT_AB + WEIGHT_BD);
    }

    TEST(ParallelDijkstra, MatchesSequentialOutput)
    {
        const CsrGraph graph = buildKnownGraph();
        const DijkstraResult serial = runSerial(graph, NODE_A);

        ThreadCountGuard guard(THREAD_COUNT_HIGH);
        const DijkstraResult parallel = GraphAnalyzer::dijkstra(graph, NODE_A);

        ASSERT_EQ(serial.distances.size(), parallel.distances.size());
        for (size_t i = 0; i < serial.distances.size(); ++i)
        {
            EXPECT_EQ(serial.distances[i], parallel.distances[i]) << "Distance divergence detected at node " << i;
        }
    }

    TEST(ParallelDijkstra, ThrowsOnInvalidSource)
    {
        const CsrGraph graph = buildKnownGraph();
        const NodeId outOfRange = graph.getNodeCount(); // first invalid index
        EXPECT_THROW((void)GraphAnalyzer::dijkstra(graph, outOfRange), std::out_of_range);
    }

    TEST(ParallelDijkstra, SingleNodeGraphReturnsZeroDistance)
    {
        const AdjacencyList adj(THREAD_COUNT_SINGLE); // 1-node graph
        const CsrGraph graph(adj);
        ThreadCountGuard guard(THREAD_COUNT_MODERATE);
        const DijkstraResult result = GraphAnalyzer::dijkstra(graph, 0);

        ASSERT_EQ(result.distances.size(), 1u);
        EXPECT_EQ(result.distances[0], 0u);
    }

    TEST(ParallelDijkstra, UnreachableNodeReturnsInfinity)
    {
        constexpr NodeId ISOLATED_NODE = 2;
        constexpr Weight EDGE_WEIGHT = 5;

        AdjacencyList adj(3);
        adj[NODE_A] = {{NODE_B, EDGE_WEIGHT}};
        const CsrGraph graph(adj);
        ThreadCountGuard guard(THREAD_COUNT_MODERATE);
        const DijkstraResult result = GraphAnalyzer::dijkstra(graph, NODE_A);

        EXPECT_EQ(result.distances[NODE_A], 0u);
        EXPECT_EQ(result.distances[NODE_B], EDGE_WEIGHT);
        EXPECT_EQ(result.distances[ISOLATED_NODE], INF_DISTANCE);
    }

    // TSan monitors this test implicitly. Passing under -fsanitize=thread
    // without warnings proves the implementation is data-race free (AC4).
    TEST(ParallelDijkstra, DataRaceFreeExecution)
    {
        AdjacencyList adj(RACE_TEST_NODE_COUNT);

        // Chain with shortcut edges to create contention on shared distance entries.
        for (NodeId u = 0; u < RACE_TEST_NODE_COUNT - 1; ++u)
        {
            adj[u].push_back({u + 1, RACE_EDGE_DIRECT});
            if (u + 2 < RACE_TEST_NODE_COUNT)
                adj[u].push_back({u + 2, RACE_EDGE_SHORTCUT});
        }

        const CsrGraph graph(adj);
        ThreadCountGuard guard(THREAD_COUNT_HIGH);

        for (int run = 0; run < RACE_TEST_RUNS; ++run)
        {
            const DijkstraResult result = GraphAnalyzer::dijkstra(graph, 0);
            EXPECT_EQ(result.distances[0], 0u);
            EXPECT_LT(result.distances[RACE_TEST_NODE_COUNT - 1], INF_DISTANCE);
        }
    }

    TEST(GraphAnalyzerParallel, ThresholdCoverageExecution)
    {
        const uint32_t numNodes = 2000;
        eop::hpc::AdjacencyList adj(numNodes);

        for (uint32_t i = 1; i < numNodes; ++i)
        {
            adj[0].push_back({i, 1});
        }

        const eop::hpc::CsrGraph massiveGraph(adj);

        auto dijkstraRes = eop::hpc::GraphAnalyzer::dijkstra(massiveGraph, 0);
        EXPECT_EQ(dijkstraRes.distances[1], 1) << "The parallel distance must be correct";
        EXPECT_EQ(dijkstraRes.distances[1999], 1);

        auto ccRes = eop::hpc::GraphAnalyzer::connectedComponents(massiveGraph);
        EXPECT_EQ(ccRes.totalComponents, 1) << "All nodes should be in a single component";
    }

} // namespace eop::hpc
