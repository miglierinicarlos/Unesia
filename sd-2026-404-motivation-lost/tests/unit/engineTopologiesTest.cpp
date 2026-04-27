#include "hpc/hpcHandler.hpp"
#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>

using namespace eop::hpc;
using json = nlohmann::json;

namespace
{
    // Protocol Constants
    constexpr size_t LENGTH_PREFIX_SIZE = 4;

    // Single Node Graph Constants
    constexpr uint32_t SINGLE_NODE_COUNT = 1;
    constexpr uint32_t SINGLE_NODE_EDGES = 0;
    constexpr uint32_t SINGLE_NODE_COMPONENTS = 1;
    constexpr uint32_t SINGLE_NODE_DISTANCE = 0;

    // Disconnected Graph Constants
    constexpr uint32_t DISCONNECTED_NODE_COUNT = 4;
    constexpr uint32_t DISCONNECTED_COMPONENTS = 3;
    constexpr int DISCONNECTED_EDGE_WEIGHT = 5;
    constexpr size_t ISOLATED_NODE_A = 2;
    constexpr size_t ISOLATED_NODE_B = 3;

    // Line Graph Constants
    constexpr int LINE_NODE_COUNT = 10;
    constexpr uint32_t LINE_EXPECTED_NODES = 10;
    constexpr uint32_t LINE_EXPECTED_EDGES = 9;
    constexpr uint32_t LINE_EXPECTED_COMPONENTS = 1;
    constexpr int LINE_EDGE_WEIGHT = 1;
    constexpr size_t LINE_LAST_NODE = 9;
    constexpr uint32_t LINE_EXPECTED_DISTANCE = 9;

    // Star Graph Constants
    constexpr uint32_t STAR_NODE_COUNT = 1000;
    constexpr uint32_t STAR_EXPECTED_EDGES = 999;
    constexpr int STAR_EDGE_WEIGHT = 2;
    constexpr size_t STAR_LAST_NODE = 999;
    constexpr size_t STAR_MIDDLE_NODE = 500;
    constexpr uint32_t STAR_EXPECTED_DISTANCE = 2;
    constexpr double MAX_CENTRALITY = 1.0;
    constexpr double MIN_CENTRALITY = 0.0;
} // namespace

static std::string stripLengthPrefix(const std::string& buffer)
{
    if (buffer.size() < LENGTH_PREFIX_SIZE)
        return "";
    return buffer.substr(LENGTH_PREFIX_SIZE);
}

TEST(EngineTopologiesTest, SingleNodeGraph)
{
    json payload;
    payload["adjacency"] = json::array();
    payload["adjacency"].push_back(json::array());

    std::string response = HpcHandler::processRequest(payload.dump(), "");
    auto parsed = json::parse(stripLengthPrefix(response));

    EXPECT_EQ(parsed["graph_size"]["nodes"], SINGLE_NODE_COUNT);
    EXPECT_EQ(parsed["graph_size"]["edges"], SINGLE_NODE_EDGES);
    EXPECT_EQ(parsed["algorithms"]["connected_components"]["total_components"], SINGLE_NODE_COMPONENTS);
    EXPECT_EQ(parsed["algorithms"]["dijkstra"]["distances"][0], SINGLE_NODE_DISTANCE);
}

TEST(EngineTopologiesTest, DisconnectedGraph)
{
    json payload;
    payload["adjacency"] = {{{{"destination", 1}, {"weight", DISCONNECTED_EDGE_WEIGHT}}},
                            {{{"destination", 0}, {"weight", DISCONNECTED_EDGE_WEIGHT}}},
                            json::array(),
                            json::array()};

    std::string response = HpcHandler::processRequest(payload.dump(), "");
    auto parsed = json::parse(stripLengthPrefix(response));

    EXPECT_EQ(parsed["graph_size"]["nodes"], DISCONNECTED_NODE_COUNT);
    EXPECT_EQ(parsed["algorithms"]["connected_components"]["total_components"], DISCONNECTED_COMPONENTS);

    EXPECT_TRUE(parsed["algorithms"]["dijkstra"]["distances"][ISOLATED_NODE_A].is_null());
    EXPECT_TRUE(parsed["algorithms"]["dijkstra"]["distances"][ISOLATED_NODE_B].is_null());
}

TEST(EngineTopologiesTest, TenNodeLineGraph)
{
    json payload;
    payload["adjacency"] = json::array();

    for (int i = 0; i < LINE_NODE_COUNT; ++i)
    {
        json edges = json::array();
        if (i < LINE_NODE_COUNT - 1)
        {
            edges.push_back({{"destination", i + 1}, {"weight", LINE_EDGE_WEIGHT}});
        }
        payload["adjacency"].push_back(edges);
    }

    std::string response = HpcHandler::processRequest(payload.dump(), "");
    auto parsed = json::parse(stripLengthPrefix(response));

    EXPECT_EQ(parsed["graph_size"]["nodes"], LINE_EXPECTED_NODES);
    EXPECT_EQ(parsed["graph_size"]["edges"], LINE_EXPECTED_EDGES);
    EXPECT_EQ(parsed["algorithms"]["connected_components"]["total_components"], LINE_EXPECTED_COMPONENTS);

    EXPECT_EQ(parsed["algorithms"]["dijkstra"]["distances"][LINE_LAST_NODE], LINE_EXPECTED_DISTANCE);
}

TEST(EngineTopologiesTest, OneThousandNodeStarGraph)
{
    json payload;
    payload["adjacency"] = json::array();

    // Nodo 0 es el centro y se conecta a todos los demas
    json centerEdges = json::array();
    for (uint32_t i = 1; i < STAR_NODE_COUNT; ++i)
    {
        centerEdges.push_back({{"destination", i}, {"weight", STAR_EDGE_WEIGHT}});
    }
    payload["adjacency"].push_back(centerEdges);

    // Los demas nodos no tienen aristas salientes
    for (uint32_t i = 1; i < STAR_NODE_COUNT; ++i)
    {
        payload["adjacency"].push_back(json::array());
    }

    std::string response = HpcHandler::processRequest(payload.dump(), "");
    auto parsed = json::parse(stripLengthPrefix(response));

    EXPECT_EQ(parsed["graph_size"]["nodes"], STAR_NODE_COUNT);
    EXPECT_EQ(parsed["graph_size"]["edges"], STAR_EXPECTED_EDGES);

    EXPECT_EQ(parsed["algorithms"]["dijkstra"]["distances"][STAR_LAST_NODE], STAR_EXPECTED_DISTANCE);

    EXPECT_DOUBLE_EQ(parsed["algorithms"]["centrality"]["values"][0], MAX_CENTRALITY);
    EXPECT_DOUBLE_EQ(parsed["algorithms"]["centrality"]["values"][STAR_MIDDLE_NODE], MIN_CENTRALITY);
}
