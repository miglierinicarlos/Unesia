#include "hpc/hpcHandler.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

using namespace eop::hpc;
using json = nlohmann::json;

namespace
{
    constexpr size_t PREFIX_SIZE = 4;
    constexpr uint32_t EXPECTED_ALGORITHM_COUNT = 3;
} // namespace

static std::string stripLengthPrefix(const std::string& buffer)
{
    if (buffer.size() < PREFIX_SIZE)
    {
        throw std::runtime_error("Buffer too small to contain length prefix");
    }

    uint32_t networkLen = 0;
    std::memcpy(&networkLen, buffer.data(), PREFIX_SIZE);
    const uint32_t hostLen = ntohl(networkLen);

    if (hostLen != buffer.size() - PREFIX_SIZE)
    {
        throw std::runtime_error("Length prefix does not match payload size");
    }

    return buffer.substr(PREFIX_SIZE);
}

TEST(HpcHandlerTest, ProcessValidPayloadDeepCheck)
{
    const std::string validJson = R"({
        "adjacency": [
            [{"destination": 1, "weight": 10}],
            [{"destination": 0, "weight": 10}],
            []
        ]
    })";

    constexpr uint32_t expectedNodes = 3;
    constexpr uint32_t expectedEdges = 2;

    testing::internal::CaptureStdout();
    const std::string response = HpcHandler::processRequest(validJson, "test-trace-id-123");
    const std::string logOutput = testing::internal::GetCapturedStdout();

    ASSERT_NO_THROW({
        const std::string pureJson = stripLengthPrefix(response);
        const auto parsed = json::parse(pureJson);

        EXPECT_TRUE(parsed.contains("algorithms"));
        EXPECT_EQ(parsed["graph_size"]["nodes"], expectedNodes);
        EXPECT_EQ(parsed["graph_size"]["edges"], expectedEdges);

        EXPECT_TRUE(parsed["processing_time_ms"].is_number_float());
        EXPECT_GT(parsed["processing_time_ms"].get<double>(), 0.0);

        EXPECT_TRUE(parsed.contains("thread_count"));
        EXPECT_TRUE(parsed["thread_count"].is_number_unsigned());
        EXPECT_GE(parsed["thread_count"].get<uint32_t>(), 1u);

        const auto& algo = parsed["algorithms"];
        EXPECT_TRUE(algo.contains("dijkstra"));
        EXPECT_TRUE(algo.contains("connected_components"));
        EXPECT_TRUE(algo.contains("centrality"));
        EXPECT_EQ(algo["dijkstra"]["distances"][1], 10u);
        EXPECT_EQ(algo["connected_components"]["total_components"], 2u);
    });

    ASSERT_NO_THROW({
        const auto logJson = json::parse(logOutput);

        EXPECT_EQ(logJson["level"], "INFO");
        EXPECT_TRUE(logJson.contains("trace_id"));
        EXPECT_TRUE(logJson.contains("span_id"));
        EXPECT_EQ(logJson["nodes"], expectedNodes);
        EXPECT_EQ(logJson["edges"], expectedEdges);
        EXPECT_GT(logJson["processing_time_ms"].get<double>(), 0.0);

        EXPECT_TRUE(logJson.contains("thread_count"));
        EXPECT_TRUE(logJson["thread_count"].is_number_unsigned());
        EXPECT_GE(logJson["thread_count"].get<uint32_t>(), 1u);

        ASSERT_TRUE(logJson.contains("algorithms_executed"));
        const auto& algorithmsExecuted = logJson.at("algorithms_executed");
        ASSERT_TRUE(algorithmsExecuted.is_array());
        EXPECT_EQ(algorithmsExecuted.size(), EXPECTED_ALGORITHM_COUNT);
        EXPECT_EQ(algorithmsExecuted.at(0), "dijkstra");
        EXPECT_EQ(algorithmsExecuted.at(1), "connected_components");
        EXPECT_EQ(algorithmsExecuted.at(2), "degree_centrality");
    });
}

TEST(HpcHandlerTest, ProcessMalformedPayloadSafely)
{
    const std::string invalidJson = R"({"adjacency": "invalid_type"})";

    testing::internal::CaptureStdout();
    const std::string response = HpcHandler::processRequest(invalidJson, "sad-path-trace");
    (void)testing::internal::GetCapturedStdout();

    ASSERT_NO_THROW({
        const std::string pureJson = stripLengthPrefix(response);
        const auto parsed = json::parse(pureJson);

        EXPECT_TRUE(parsed.contains("error"));
        EXPECT_NE(parsed["error"].get<std::string>().find("Malformed payload"), std::string::npos);
    });
}
