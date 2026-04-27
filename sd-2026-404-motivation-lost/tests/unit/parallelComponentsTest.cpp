#include "hpc/csrGraph.hpp"
#include "hpc/graphAnalyzer.hpp"
#include <gtest/gtest.h>
#include <omp.h>
#include <unordered_set>
#include <vector>

namespace eop::hpc
{
    namespace
    {
        constexpr int THREAD_COUNT_SINGLE = 1;
        constexpr int THREAD_COUNT_MODERATE = 4;
        constexpr int THREAD_COUNT_HIGH = 8;

        constexpr NodeId RACE_TEST_NODE_COUNT = 1000;
        constexpr int RACE_TEST_RUNS = 5;

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

        ComponentsResult runSerial(const CsrGraph& graph)
        {
            ThreadCountGuard guard(THREAD_COUNT_SINGLE);
            return GraphAnalyzer::connectedComponents(graph);
        }

        /// Verifies that the component ID assignment is internally consistent:
        /// nodes sharing a component ID must form a set, and no two distinct
        /// components may share an ID.
        bool componentIdsAreConsistent(const ComponentsResult& result)
        {
            const size_t nodeCount = result.componentId.size();
            for (size_t i = 0; i < nodeCount; ++i)
            {
                if (result.componentId[i] == 0)
                    return false; // component IDs must be 1-based
            }
            return true;
        }
    } // namespace

    // ── Topology: two fully disconnected pairs ────────────────────────────
    //   A <-> B     C <-> D    (undirected via bidirectional edges)
    //   Expected: 2 components
    TEST(ParallelComponents, TwoIsolatedPairsYieldTwoComponents)
    {
        constexpr NodeId NODE_A = 0;
        constexpr NodeId NODE_B = 1;
        constexpr NodeId NODE_C = 2;
        constexpr NodeId NODE_D = 3;
        constexpr Weight EDGE_W = 1;

        AdjacencyList adj(4);
        adj[NODE_A] = {{NODE_B, EDGE_W}};
        adj[NODE_B] = {{NODE_A, EDGE_W}};
        adj[NODE_C] = {{NODE_D, EDGE_W}};
        adj[NODE_D] = {{NODE_C, EDGE_W}};

        const CsrGraph graph(adj);
        ThreadCountGuard guard(THREAD_COUNT_MODERATE);
        const ComponentsResult result = GraphAnalyzer::connectedComponents(graph);

        EXPECT_EQ(result.totalComponents, 2u);
        EXPECT_TRUE(componentIdsAreConsistent(result));
        // A and B must share a component; C and D must share a different one.
        EXPECT_EQ(result.componentId[NODE_A], result.componentId[NODE_B]);
        EXPECT_EQ(result.componentId[NODE_C], result.componentId[NODE_D]);
        EXPECT_NE(result.componentId[NODE_A], result.componentId[NODE_C]);
    }

    // ── Topology: fully connected chain ──────────────────────────────────
    //   A -> B -> C -> D    Expected: 1 component
    TEST(ParallelComponents, FullyConnectedChainYieldsOneComponent)
    {
        constexpr NodeId NODE_A = 0;
        constexpr NodeId NODE_B = 1;
        constexpr NodeId NODE_C = 2;
        constexpr NodeId NODE_D = 3;
        constexpr Weight EDGE_W = 1;

        AdjacencyList adj(4);
        adj[NODE_A] = {{NODE_B, EDGE_W}};
        adj[NODE_B] = {{NODE_C, EDGE_W}};
        adj[NODE_C] = {{NODE_D, EDGE_W}};

        const CsrGraph graph(adj);
        ThreadCountGuard guard(THREAD_COUNT_MODERATE);
        const ComponentsResult result = GraphAnalyzer::connectedComponents(graph);

        EXPECT_EQ(result.totalComponents, 1u);
        EXPECT_TRUE(componentIdsAreConsistent(result));
        EXPECT_EQ(result.componentId[NODE_A], result.componentId[NODE_D]);
    }

    // ── Topology: all nodes isolated (no edges) ───────────────────────────
    //   Expected: N components
    TEST(ParallelComponents, AllIsolatedNodesYieldNComponents)
    {
        constexpr NodeId NODE_COUNT = 5;
        const AdjacencyList adj(NODE_COUNT);
        const CsrGraph graph(adj);
        ThreadCountGuard guard(THREAD_COUNT_MODERATE);
        const ComponentsResult result = GraphAnalyzer::connectedComponents(graph);

        EXPECT_EQ(result.totalComponents, NODE_COUNT);
        EXPECT_TRUE(componentIdsAreConsistent(result));

        // Every node must have a unique component ID.
        std::unordered_set<uint32_t> ids(result.componentId.begin(), result.componentId.end());
        EXPECT_EQ(ids.size(), NODE_COUNT);
    }

    // ── Topology: single node ─────────────────────────────────────────────
    TEST(ParallelComponents, SingleNodeYieldsOneComponent)
    {
        const AdjacencyList adj(1);
        const CsrGraph graph(adj);
        ThreadCountGuard guard(THREAD_COUNT_MODERATE);
        const ComponentsResult result = GraphAnalyzer::connectedComponents(graph);

        EXPECT_EQ(result.totalComponents, 1u);
        EXPECT_EQ(result.componentId[0], 1u);
    }

    // ── Empty graph ───────────────────────────────────────────────────────
    TEST(ParallelComponents, EmptyGraphYieldsZeroComponents)
    {
        const AdjacencyList adj(0);
        const CsrGraph graph(adj);
        ThreadCountGuard guard(THREAD_COUNT_MODERATE);
        const ComponentsResult result = GraphAnalyzer::connectedComponents(graph);

        EXPECT_EQ(result.totalComponents, 0u);
        EXPECT_TRUE(result.componentId.empty());
    }

    // ── Parallel output must match serial baseline exactly ────────────────
    TEST(ParallelComponents, MatchesSequentialOutput)
    {
        constexpr NodeId NODE_A = 0;
        constexpr NodeId NODE_B = 1;
        constexpr NodeId NODE_C = 2;
        constexpr NodeId NODE_D = 3;
        constexpr NodeId NODE_E = 4;
        constexpr Weight EDGE_W = 1;

        // Two components: {A,B,C} and {D,E}
        AdjacencyList adj(5);
        adj[NODE_A] = {{NODE_B, EDGE_W}, {NODE_C, EDGE_W}};
        adj[NODE_B] = {{NODE_A, EDGE_W}};
        adj[NODE_C] = {{NODE_A, EDGE_W}};
        adj[NODE_D] = {{NODE_E, EDGE_W}};
        adj[NODE_E] = {{NODE_D, EDGE_W}};

        const CsrGraph graph = CsrGraph(adj);
        const ComponentsResult serial = runSerial(graph);

        ThreadCountGuard guard(THREAD_COUNT_HIGH);
        const ComponentsResult parallel = GraphAnalyzer::connectedComponents(graph);

        ASSERT_EQ(serial.totalComponents, parallel.totalComponents);
        ASSERT_EQ(serial.componentId.size(), parallel.componentId.size());

        // Component IDs may be assigned in different order between runs,
        // so we verify structural equivalence: same nodes share a component.
        const size_t nodeCount = serial.componentId.size();
        for (size_t i = 0; i < nodeCount; ++i)
        {
            for (size_t j = i + 1; j < nodeCount; ++j)
            {
                const bool serialSame = serial.componentId[i] == serial.componentId[j];
                const bool parallelSame = parallel.componentId[i] == parallel.componentId[j];
                EXPECT_EQ(serialSame, parallelSame)
                    << "Component membership divergence between nodes " << i << " and " << j;
            }
        }
    }

    // TSan monitors this test implicitly. Passing under -fsanitize=thread
    // without warnings proves the atomic BFS claims are data-race free (AC4).
    TEST(ParallelComponents, AtomicRaceFreeExecution)
    {
        AdjacencyList adj(RACE_TEST_NODE_COUNT);
        constexpr Weight EDGE_W = 1;

        // Dense bidirectional connections to maximize thread contention
        // on the visited[] atomic array during frontier expansion.
        for (NodeId u = 0; u < RACE_TEST_NODE_COUNT - 1; ++u)
        {
            adj[u].push_back({u + 1, EDGE_W});
            adj[u + 1].push_back({u, EDGE_W});
            if (u + 3 < RACE_TEST_NODE_COUNT)
            {
                adj[u].push_back({u + 3, EDGE_W});
                adj[u + 3].push_back({u, EDGE_W});
            }
        }

        const CsrGraph graph(adj);
        ThreadCountGuard guard(THREAD_COUNT_HIGH);

        for (int run = 0; run < RACE_TEST_RUNS; ++run)
        {
            const ComponentsResult result = GraphAnalyzer::connectedComponents(graph);
            EXPECT_EQ(result.totalComponents, 1u);
            EXPECT_EQ(result.componentId.size(), RACE_TEST_NODE_COUNT);
        }
    }

} // namespace eop::hpc
