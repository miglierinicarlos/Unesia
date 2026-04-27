#include "hpc/graphAnalyzer.hpp"
#include <gtest/gtest.h>

using namespace eop::hpc;

namespace
{
    // Dijkstra Correctness Constants
    constexpr uint32_t DIJK_TOTAL_NODES = 4;
    constexpr NodeId DIJK_N0 = 0, DIJK_N1 = 1, DIJK_N2 = 2, DIJK_N3 = 3;
    constexpr Weight DIJK_W01 = 1, DIJK_W02 = 4, DIJK_W12 = 2, DIJK_W13 = 6, DIJK_W23 = 3;
    constexpr uint32_t DIJK_DIST0 = 0, DIJK_DIST1 = 1, DIJK_DIST2 = 3, DIJK_DIST3 = 6;

    // Dijkstra Validation Constants
    constexpr uint32_t DIJK_VAL_TOTAL_NODES = 1;
    constexpr NodeId DIJK_VAL_OUT_OF_BOUNDS = 5;

    // Connected Components Constants
    constexpr uint32_t CC_TOTAL_NODES = 5;
    constexpr NodeId CC_N0 = 0, CC_N1 = 1, CC_N2 = 2, CC_N3 = 3, CC_N4 = 4;
    constexpr Weight CC_WEIGHT = 1;
    constexpr uint32_t CC_EXPECTED_COMPONENTS = 3;

    // Degree Centrality Constants
    constexpr uint32_t CENT_TOTAL_NODES = 3;
    constexpr NodeId CENT_N0 = 0, CENT_N1 = 1, CENT_N2 = 2;
    constexpr Weight CENT_W01 = 10, CENT_W02 = 20, CENT_W12 = 30;
    constexpr double CENT_VAL0 = 1.0, CENT_VAL1 = 0.5, CENT_VAL2 = 0.0;

    // Degree Centrality Edge Cases Constants
    constexpr uint32_t CENT_EDGE_EMPTY_NODES = 0;
    constexpr uint32_t CENT_EDGE_SINGLE_NODES = 1;
    constexpr double CENT_EDGE_SINGLE_VAL = 0.0;

    // Medium Graph Constants
    constexpr uint32_t MED_TOTAL_NODES = 1000;
    constexpr Weight MED_EDGE_WEIGHT = 1;
    constexpr NodeId MED_SOURCE_NODE = 0;
    constexpr NodeId MED_TARGET_NODE = 999;
    constexpr uint32_t MED_EXPECTED_DIST = 999;
} // namespace

// Helper to construct a CsrGraph easily within tests
static CsrGraph buildTestGraph(const AdjacencyList& adj)
{
    return CsrGraph(adj);
}

// ----------------------------------------------------------------------------
// DIJKSTRA TESTS
// ----------------------------------------------------------------------------

TEST(AlgorithmsTest, DijkstraCorrectness)
{
    AdjacencyList adj(DIJK_TOTAL_NODES);
    adj[DIJK_N0] = {{DIJK_N1, DIJK_W01}, {DIJK_N2, DIJK_W02}};
    adj[DIJK_N1] = {{DIJK_N2, DIJK_W12}, {DIJK_N3, DIJK_W13}};
    adj[DIJK_N2] = {{DIJK_N3, DIJK_W23}};
    adj[DIJK_N3] = {};

    const CsrGraph graph = buildTestGraph(adj);
    const DijkstraResult result = GraphAnalyzer::dijkstra(graph, DIJK_N0);

    EXPECT_EQ(result.sourceNode, DIJK_N0);
    EXPECT_EQ(result.distances.size(), DIJK_TOTAL_NODES);

    EXPECT_EQ(result.distances[DIJK_N0], DIJK_DIST0);
    EXPECT_EQ(result.distances[DIJK_N1], DIJK_DIST1);
    EXPECT_EQ(result.distances[DIJK_N2], DIJK_DIST2);
    EXPECT_EQ(result.distances[DIJK_N3], DIJK_DIST3);
}

TEST(AlgorithmsTest, DijkstraInputValidation)
{
    AdjacencyList adj(DIJK_VAL_TOTAL_NODES);
    const CsrGraph graph = buildTestGraph(adj);

    EXPECT_THROW(static_cast<void>(GraphAnalyzer::dijkstra(graph, DIJK_VAL_OUT_OF_BOUNDS)), std::out_of_range);
}

TEST(AlgorithmsTest, ConnectedComponentsCorrectness)
{
    AdjacencyList adj(CC_TOTAL_NODES);

    adj[CC_N0] = {{CC_N1, CC_WEIGHT}};
    adj[CC_N1] = {{CC_N0, CC_WEIGHT}};

    adj[CC_N2] = {{CC_N3, CC_WEIGHT}};
    adj[CC_N3] = {{CC_N2, CC_WEIGHT}};

    adj[CC_N4] = {};

    const CsrGraph graph = buildTestGraph(adj);
    const ComponentsResult result = GraphAnalyzer::connectedComponents(graph);

    ASSERT_EQ(result.componentId.size(), CC_TOTAL_NODES);
    EXPECT_EQ(result.totalComponents, CC_EXPECTED_COMPONENTS);

    EXPECT_EQ(result.componentId[CC_N0], result.componentId[CC_N1]);
    EXPECT_EQ(result.componentId[CC_N2], result.componentId[CC_N3]);
    EXPECT_NE(result.componentId[CC_N0], result.componentId[CC_N2]);
    EXPECT_NE(result.componentId[CC_N0], result.componentId[CC_N4]);
}

TEST(AlgorithmsTest, DegreeCentralityCorrectness)
{
    AdjacencyList adj(CENT_TOTAL_NODES);
    adj[CENT_N0] = {{CENT_N1, CENT_W01}, {CENT_N2, CENT_W02}};
    adj[CENT_N1] = {{CENT_N2, CENT_W12}};
    adj[CENT_N2] = {};

    const CsrGraph graph = buildTestGraph(adj);
    const CentralityResult result = GraphAnalyzer::degreeCentrality(graph);

    ASSERT_EQ(result.centrality.size(), CENT_TOTAL_NODES);

    EXPECT_DOUBLE_EQ(result.centrality[CENT_N0], CENT_VAL0);
    EXPECT_DOUBLE_EQ(result.centrality[CENT_N1], CENT_VAL1);
    EXPECT_DOUBLE_EQ(result.centrality[CENT_N2], CENT_VAL2);
}

TEST(AlgorithmsTest, DegreeCentralityEdgeCases)
{
    // Empty Graph (0 nodes)
    AdjacencyList adj0(CENT_EDGE_EMPTY_NODES);
    const CsrGraph graph0 = buildTestGraph(adj0);
    EXPECT_TRUE(GraphAnalyzer::degreeCentrality(graph0).centrality.empty());

    // Graph with 1 node
    AdjacencyList adj1(CENT_EDGE_SINGLE_NODES);
    const CsrGraph graph1 = buildTestGraph(adj1);
    const CentralityResult res1 = GraphAnalyzer::degreeCentrality(graph1);

    ASSERT_EQ(res1.centrality.size(), CENT_EDGE_SINGLE_NODES);
    EXPECT_DOUBLE_EQ(res1.centrality[0], CENT_EDGE_SINGLE_VAL);
}

TEST(AlgorithmsTest, DijkstraMediumGraphOneThousandNodes)
{
    AdjacencyList adj(MED_TOTAL_NODES);
    for (uint32_t i = 0; i + 1 < MED_TOTAL_NODES; ++i)
    {
        adj[i].push_back({i + 1, MED_EDGE_WEIGHT});
    }

    const CsrGraph graph = buildTestGraph(adj);
    const DijkstraResult result = GraphAnalyzer::dijkstra(graph, MED_SOURCE_NODE);

    ASSERT_EQ(result.distances.size(), MED_TOTAL_NODES);
    EXPECT_EQ(result.distances[MED_SOURCE_NODE], 0u);
    EXPECT_EQ(result.distances[MED_TARGET_NODE], MED_EXPECTED_DIST);
}
