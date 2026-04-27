#include "hpc/csrGraph.hpp"
#include <gtest/gtest.h>

using namespace eop::hpc;

namespace
{
    // Allocation & Layout Constants
    constexpr uint32_t ALLOC_TOTAL_NODES = 3;
    constexpr uint32_t ALLOC_TOTAL_EDGES = 3;
    constexpr NodeId ALLOC_N0 = 0, ALLOC_N1 = 1, ALLOC_N2 = 2;
    constexpr Weight ALLOC_W01 = 10, ALLOC_W02 = 20, ALLOC_W12 = 30;
    constexpr uint32_t ALLOC_DEG0 = 2, ALLOC_DEG1 = 1, ALLOC_DEG2 = 0;

    // Immutability Constants
    constexpr uint32_t IMMUT_TOTAL_NODES = 2;
    constexpr NodeId IMMUT_N0 = 0, IMMUT_N1 = 1;
    constexpr Weight IMMUT_WEIGHT = 5;
    constexpr uint32_t IMMUT_DEG0 = 1;

    // Empty Graph Constants
    constexpr uint32_t EMPTY_NO_NODES = 0;
    constexpr uint32_t EMPTY_NO_EDGES = 0;
    constexpr uint32_t EMPTY_ISOLATED_TOTAL = 5;
    constexpr NodeId EMPTY_ISOLATED_NODE = 3;
    constexpr uint32_t EMPTY_ISOLATED_DEG = 0;

    // Validation Constants
    constexpr uint32_t VAL_SINGLE_NODE = 1;
    constexpr NodeId VAL_OUT_OF_RANGE = 5;
    constexpr Weight VAL_ARBITRARY_WEIGHT = 100;
} // namespace

TEST(CsrGraphTest, AllocationAndLayout)
{
    AdjacencyList adj(ALLOC_TOTAL_NODES);
    adj[ALLOC_N0] = {{ALLOC_N1, ALLOC_W01}, {ALLOC_N2, ALLOC_W02}};
    adj[ALLOC_N1] = {{ALLOC_N2, ALLOC_W12}};

    CsrGraph graph(adj);

    EXPECT_EQ(graph.getNodeCount(), ALLOC_TOTAL_NODES);
    EXPECT_EQ(graph.getEdgeCount(), ALLOC_TOTAL_EDGES);

    EXPECT_EQ(graph.getNodeDegree(ALLOC_N0), ALLOC_DEG0);
    EXPECT_EQ(graph.getNodeDegree(ALLOC_N1), ALLOC_DEG1);
    EXPECT_EQ(graph.getNodeDegree(ALLOC_N2), ALLOC_DEG2);

    const auto* neighbors0 = graph.getNeighbors(ALLOC_N0);
    const auto* weights0 = graph.getWeights(ALLOC_N0);
    EXPECT_EQ(neighbors0[0], ALLOC_N1);
    EXPECT_EQ(weights0[0], ALLOC_W01);
    EXPECT_EQ(neighbors0[1], ALLOC_N2);
    EXPECT_EQ(weights0[1], ALLOC_W02);

    const auto* neighbors1 = graph.getNeighbors(ALLOC_N1);
    const auto* weights1 = graph.getWeights(ALLOC_N1);
    EXPECT_EQ(neighbors1[0], ALLOC_N2);
    EXPECT_EQ(weights1[0], ALLOC_W12);
}

TEST(CsrGraphTest, ImmutabilityAndAccess)
{
    AdjacencyList adj(IMMUT_TOTAL_NODES);
    adj[IMMUT_N0] = {{IMMUT_N1, IMMUT_WEIGHT}};
    adj[IMMUT_N1] = {{IMMUT_N0, IMMUT_WEIGHT}};

    const CsrGraph graph(adj); // const instance validates const-correctness

    EXPECT_EQ(graph.getNodeDegree(IMMUT_N0), IMMUT_DEG0);
    EXPECT_EQ(graph.getNeighbors(IMMUT_N0)[0], IMMUT_N1);
    EXPECT_EQ(graph.getWeights(IMMUT_N0)[0], IMMUT_WEIGHT);
}

TEST(CsrGraphTest, EmptyGraphHandling)
{
    CsrGraph emptyGraph {AdjacencyList(EMPTY_NO_NODES)};
    EXPECT_EQ(emptyGraph.getNodeCount(), EMPTY_NO_NODES);
    EXPECT_EQ(emptyGraph.getEdgeCount(), EMPTY_NO_EDGES);

    CsrGraph disconnectedGraph {AdjacencyList(EMPTY_ISOLATED_TOTAL)};
    EXPECT_EQ(disconnectedGraph.getNodeCount(), EMPTY_ISOLATED_TOTAL);
    EXPECT_EQ(disconnectedGraph.getEdgeCount(), EMPTY_NO_EDGES);
    EXPECT_EQ(disconnectedGraph.getNodeDegree(EMPTY_ISOLATED_NODE), EMPTY_ISOLATED_DEG);

    EXPECT_EQ(disconnectedGraph.getNeighbors(EMPTY_ISOLATED_NODE), nullptr);
    EXPECT_EQ(disconnectedGraph.getWeights(EMPTY_ISOLATED_NODE), nullptr);

    const NodeId outOfRange = EMPTY_ISOLATED_TOTAL + 1;
    EXPECT_EQ(disconnectedGraph.getNeighbors(outOfRange), nullptr);
    EXPECT_EQ(disconnectedGraph.getWeights(outOfRange), nullptr);
}

TEST(CsrGraphTest, InputValidation)
{
    AdjacencyList adj(VAL_SINGLE_NODE);
    adj[0] = {{VAL_OUT_OF_RANGE, VAL_ARBITRARY_WEIGHT}};

    EXPECT_THROW({ CsrGraph graph {adj}; }, std::out_of_range);
}
