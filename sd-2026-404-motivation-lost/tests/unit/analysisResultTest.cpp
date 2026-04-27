#include "hpc/analysisResult.hpp"
#include <gtest/gtest.h>
#include <limits>
#include <nlohmann/json.hpp>

using namespace eop::hpc;
using json = nlohmann::json;

namespace
{
    // Metadata Constants
    constexpr uint32_t STD_NODES = 3;
    constexpr uint32_t STD_EDGES = 2;
    constexpr double STD_TIME_MS = 12.5;
    constexpr uint32_t STD_THREADS = 1;

    // Dijkstra Constants
    constexpr uint32_t UNREACHABLE = std::numeric_limits<uint32_t>::max();
    constexpr uint32_t DIJK_SOURCE = 0;
    constexpr uint32_t DIJK_DIST_1 = 10;

    // Connected Components Constants
    constexpr uint32_t COMP_TOTAL = 2;
    constexpr uint32_t COMP_ID_0 = 1;
    constexpr uint32_t COMP_ID_1 = 1;
    constexpr uint32_t COMP_ID_2 = 2;

    // Centrality Constants
    constexpr double CENT_0 = 1.0;
    constexpr double CENT_1 = 0.5;
    constexpr double CENT_2 = 0.0;

    // Empty Graph Constants
    constexpr uint32_t EMPTY_COUNT = 0;
} // namespace

// ---------------------------------------------------------------------------
// Helpers & Fixtures
// ---------------------------------------------------------------------------
static AnalysisResult makeStandardResult()
{
    AnalysisResult r;
    r.graphSize = {STD_NODES, STD_EDGES};
    r.processingTimeMs = STD_TIME_MS;
    r.threadCount = STD_THREADS;

    r.dijkstra = {DIJK_SOURCE, {DIJK_SOURCE, DIJK_DIST_1, UNREACHABLE}};
    r.components = {COMP_TOTAL, {COMP_ID_0, COMP_ID_1, COMP_ID_2}};
    r.centrality = {{CENT_0, CENT_1, CENT_2}};

    return r;
}

class AnalysisResultTest : public ::testing::Test
{
protected:
    AnalysisResult baseResult = makeStandardResult();
};

// ---------------------------------------------------------------------------
// Test Suites
// ---------------------------------------------------------------------------

TEST_F(AnalysisResultTest, SerializesMetadataAndSchemaCorrectly)
{
    const std::string jsonString = baseResult.toJson();

    ASSERT_NO_THROW({
        const auto parsed = json::parse(jsonString);

        // Top-level schema validation
        EXPECT_TRUE(parsed.contains("algorithms"));
        EXPECT_TRUE(parsed.contains("graph_size"));

        // Metadata validation
        EXPECT_EQ(parsed["graph_size"]["nodes"], STD_NODES);
        EXPECT_EQ(parsed["graph_size"]["edges"], STD_EDGES);
        EXPECT_DOUBLE_EQ(parsed["processing_time_ms"], STD_TIME_MS);
        EXPECT_EQ(parsed["thread_count"], STD_THREADS);
    });
}

TEST_F(AnalysisResultTest, SerializesAlgorithmsAndHandlesNullptr)
{
    const auto parsed = json::parse(baseResult.toJson());

    // Dijkstra validation
    EXPECT_EQ(parsed["algorithms"]["dijkstra"]["source_node"], DIJK_SOURCE);
    EXPECT_EQ(parsed["algorithms"]["dijkstra"]["distances"][1], DIJK_DIST_1);
    EXPECT_TRUE(parsed["algorithms"]["dijkstra"]["distances"][2].is_null());

    // Components & Centrality validation
    EXPECT_EQ(parsed["algorithms"]["connected_components"]["total_components"], COMP_TOTAL);
    EXPECT_EQ(parsed["algorithms"]["connected_components"]["component_id"][2], COMP_ID_2);
    EXPECT_DOUBLE_EQ(parsed["algorithms"]["centrality"]["values"][1], CENT_1);
}

TEST_F(AnalysisResultTest, HandlesEmptyAndDisconnectedGraphsSafely)
{
    AnalysisResult emptyResult;
    emptyResult.graphSize = {EMPTY_COUNT, EMPTY_COUNT};
    emptyResult.processingTimeMs = 0.0;
    emptyResult.threadCount = 1;

    std::string jsonStr;

    EXPECT_NO_THROW({ jsonStr = emptyResult.toJson(); });

    const auto parsed = json::parse(jsonStr);

    EXPECT_TRUE(parsed["algorithms"]["dijkstra"]["distances"].is_array());
    EXPECT_TRUE(parsed["algorithms"]["dijkstra"]["distances"].empty());

    EXPECT_TRUE(parsed["algorithms"]["connected_components"]["component_id"].is_array());
    EXPECT_TRUE(parsed["algorithms"]["connected_components"]["component_id"].empty());

    EXPECT_EQ(parsed["graph_size"]["nodes"], EMPTY_COUNT);
    EXPECT_EQ(parsed["graph_size"]["edges"], EMPTY_COUNT);
}
