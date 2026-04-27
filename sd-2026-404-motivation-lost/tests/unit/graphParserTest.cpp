#include "hpc/graphParser.hpp"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>

using namespace eop::hpc;
using json = nlohmann::json;

namespace
{
    // Valid Graph Constants
    constexpr uint32_t VALID_EXPECTED_NODES = 3;
    constexpr uint32_t VALID_EXPECTED_EDGES = 3;
    constexpr NodeId VALID_N0 = 0, VALID_N1 = 1, VALID_N2 = 2;
    constexpr Weight VALID_W01 = 10, VALID_W02 = 20, VALID_W12 = 30;
    constexpr uint32_t VALID_N0_DEGREE = 2;

    // Invalid Topology Constants
    constexpr NodeId INVALID_DESTINATION = 5;
    constexpr Weight INVALID_WEIGHT = 10;

    // Invalid Schema Constants
    constexpr int NEGATIVE_WEIGHT = -5;
    constexpr int INVALID_NODE_TYPE_VAL = 12345;
} // namespace

TEST(GraphParserTest, ParseValidAdjacencyList)
{
    json payload;
    payload["adjacency"] = {
        {{{"destination", VALID_N1}, {"weight", VALID_W01}}, {{"destination", VALID_N2}, {"weight", VALID_W02}}},
        {{{"destination", VALID_N2}, {"weight", VALID_W12}}},
        json::array()};

    ParseResult result = GraphParser::parse(payload.dump());

    ASSERT_TRUE(result.isSuccess()) << "Failed to parse valid JSON: " << result.errorMessage;
    ASSERT_TRUE(result.graph.has_value());
    if (!result.graph.has_value())
    {
        return;
    }
    const auto& graph = result.graph.value();

    EXPECT_EQ(graph.getNodeCount(), VALID_EXPECTED_NODES);
    EXPECT_EQ(graph.getEdgeCount(), VALID_EXPECTED_EDGES);
    EXPECT_EQ(graph.getNodeDegree(VALID_N0), VALID_N0_DEGREE);
    EXPECT_EQ(graph.getNeighbors(VALID_N1)[0], VALID_N2);
}

TEST(GraphParserTest, RejectMalformedJsonSyntax)
{
    // Raw string is mandatory here to deliberately simulate a broken JSON transmission
    const std::string malformedJson = R"({ "adjacency": [ )";

    ParseResult result = GraphParser::parse(malformedJson);

    EXPECT_FALSE(result.isSuccess());
    EXPECT_FALSE(result.graph.has_value());
    EXPECT_NE(result.errorMessage.find("JSON Parse Error"), std::string::npos);
}

TEST(GraphParserTest, RejectInvalidSchema)
{
    const std::string invalidSchemaJson = R"({"adjacency": "not_an_array"})";

    ParseResult result = GraphParser::parse(invalidSchemaJson);

    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.errorMessage, "Malformed payload: Missing or invalid 'adjacency' array");
}

TEST(GraphParserTest, RejectInvalidTopology)
{
    json payload;
    payload["adjacency"] = {{{{"destination", INVALID_DESTINATION}, {"weight", INVALID_WEIGHT}}}, json::array()};

    ParseResult result = GraphParser::parse(payload.dump());

    EXPECT_FALSE(result.isSuccess());
    EXPECT_NE(result.errorMessage.find("Invalid Topology"), std::string::npos);
}

TEST(GraphParserTest, RejectNegativeWeightsOrTypes)
{
    json payload;
    payload["adjacency"] = {{{{"destination", VALID_N1}, {"weight", NEGATIVE_WEIGHT}}}};

    ParseResult result = GraphParser::parse(payload.dump());

    EXPECT_FALSE(result.isSuccess());
    EXPECT_NE(result.errorMessage.find("unsigned"), std::string::npos);
}

TEST(GraphParserTest, RejectNodeEdgesNotArray)
{
    json payload;
    payload["adjacency"] = {INVALID_NODE_TYPE_VAL};

    ParseResult result = GraphParser::parse(payload.dump());

    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.errorMessage, "Malformed payload: Node edges must be an array");
}
