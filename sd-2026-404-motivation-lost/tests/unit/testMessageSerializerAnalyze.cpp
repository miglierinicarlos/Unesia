#include "messageSerializer.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace
{
    // Sends one complete ADR-003 frame over the write end of a socketpair.
    void sendFrameAsync(int writeFd, MessageSerializer::MessageType type, uint32_t msgId, const std::string& payload)
    {
        const auto frame = MessageSerializer::buildFrame(type, msgId, payload);
        const bool sent = MessageSerializer::sendAll(writeFd, frame.data(), frame.size());
        (void)sent;
    }
} // namespace

// ---------------------------------------------------------------------------
// MessageSerializerAnalyze.BuildAnalyzeGraphPayload_RoundTrip
// Serializes an ANALYZE_GRAPH payload, wraps it in a frame, sends it over a
// socketpair, reads it back with readFrame(), and verifies every field.
// ---------------------------------------------------------------------------
TEST(MessageSerializerAnalyze, BuildAnalyzeGraphPayload_RoundTrip)
{
    MessageSerializer::AnalyzeGraphPayloadData input;
    input.algorithm = "dijkstra";
    input.sourceNode = 1U;
    input.graph = {{"1", {"2", "3"}}, {"2", {"1"}}, {"3", {"1"}}};
    input.traceId = "trace-abc-123";

    const std::string payload = MessageSerializer::buildAnalyzeGraphPayload(input);
    constexpr uint32_t MSG_ID = 42U;

    std::array<int, 2> sv = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()), 0);

    std::thread writer([&]()
                       { sendFrameAsync(sv[1], MessageSerializer::MessageType::ANALYZE_GRAPH, MSG_ID, payload); });

    const auto result = MessageSerializer::readFrame(sv[0]);
    writer.join();

    EXPECT_EQ(result.status, MessageSerializer::FrameReadStatus::OK);
    EXPECT_EQ(result.messageType, MessageSerializer::MessageType::ANALYZE_GRAPH);
    EXPECT_EQ(result.messageId, MSG_ID);

    const auto parsed = MessageSerializer::parseAnalyzeGraphPayload(result.payload);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->algorithm, "dijkstra");
    ASSERT_TRUE(parsed->sourceNode.has_value());
    EXPECT_EQ(parsed->sourceNode.value(), 1U);
    EXPECT_EQ(parsed->traceId, "trace-abc-123");

    ASSERT_EQ(parsed->graph.size(), input.graph.size());
    EXPECT_EQ(parsed->graph.at("1").size(), input.graph.at("1").size());

    ::close(sv[0]);
    ::close(sv[1]);
}

// ---------------------------------------------------------------------------
// MessageSerializerAnalyze.BuildAnalyzeGraphPayload_NoSourceNode_RoundTrip
// Verifies that source_node is omitted from the payload when not set and that
// the parsed result reflects the absent optional.
// ---------------------------------------------------------------------------
TEST(MessageSerializerAnalyze, BuildAnalyzeGraphPayload_NoSourceNode_RoundTrip)
{
    MessageSerializer::AnalyzeGraphPayloadData input;
    input.algorithm = "connected_components";
    // sourceNode intentionally left empty
    input.graph = {{"A", {"B"}}, {"B", {"A", "C"}}, {"C", {"B"}}};
    input.traceId = "trace-xyz-456";

    const std::string payload = MessageSerializer::buildAnalyzeGraphPayload(input);

    // source_node must be absent from the serialized JSON
    const auto j = nlohmann::json::parse(payload);
    EXPECT_FALSE(j.contains("source_node"));

    const auto parsed = MessageSerializer::parseAnalyzeGraphPayload(payload);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->sourceNode.has_value());
    EXPECT_EQ(parsed->algorithm, "connected_components");
}

// ---------------------------------------------------------------------------
// MessageSerializerAnalyze.BuildAnalyzeResultPayload_RoundTrip
// Serializes an ANALYZE_RESULT payload, sends it over a socketpair, reads it
// back with readFrame(), and verifies every field.
// ---------------------------------------------------------------------------
TEST(MessageSerializerAnalyze, BuildAnalyzeResultPayload_RoundTrip)
{
    MessageSerializer::AnalyzeResultPayloadData input;
    input.graphSize = {.nodes = 100U, .edges = 200U};
    input.processingTimeMs = 38U;
    input.threadCount = 4U;
    input.algorithms = {{.algorithm = "dijkstra", .resultJson = R"({"distances":{"1":0,"2":1,"3":2}})"},
                        {.algorithm = "centrality", .resultJson = R"({"scores":{"1":0.9,"2":0.5,"3":0.3}})"}};

    const std::string payload = MessageSerializer::buildAnalyzeResultPayload(input);
    constexpr uint32_t MSG_ID = 7U;

    std::array<int, 2> sv = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()), 0);

    std::thread writer([&]()
                       { sendFrameAsync(sv[1], MessageSerializer::MessageType::ANALYZE_RESULT, MSG_ID, payload); });

    const auto result = MessageSerializer::readFrame(sv[0]);
    writer.join();

    EXPECT_EQ(result.status, MessageSerializer::FrameReadStatus::OK);
    EXPECT_EQ(result.messageType, MessageSerializer::MessageType::ANALYZE_RESULT);
    EXPECT_EQ(result.messageId, MSG_ID);

    const auto parsed = MessageSerializer::parseAnalyzeResultPayload(result.payload);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->graphSize.nodes, 100U);
    EXPECT_EQ(parsed->graphSize.edges, 200U);
    EXPECT_EQ(parsed->processingTimeMs, 38U);
    EXPECT_EQ(parsed->threadCount, 4U);
    ASSERT_EQ(parsed->algorithms.size(), 2U);
    EXPECT_EQ(parsed->algorithms[0].algorithm, "dijkstra");
    EXPECT_EQ(parsed->algorithms[1].algorithm, "centrality");

    ::close(sv[0]);
    ::close(sv[1]);
}

// ---------------------------------------------------------------------------
// MessageSerializerAnalyze.AnalyzeGraph_MalformedPayload
// Passes an invalid JSON string to parseAnalyzeGraphPayload() and expects
// std::nullopt to be returned.
// ---------------------------------------------------------------------------
TEST(MessageSerializerAnalyze, AnalyzeGraph_MalformedPayload)
{
    // Completely invalid JSON
    EXPECT_FALSE(MessageSerializer::parseAnalyzeGraphPayload("not-json").has_value());

    // Valid JSON but missing required fields
    EXPECT_FALSE(MessageSerializer::parseAnalyzeGraphPayload(R"({"algorithm":"dijkstra"})").has_value());

    // Empty string
    EXPECT_FALSE(MessageSerializer::parseAnalyzeGraphPayload("").has_value());
}

// ---------------------------------------------------------------------------
// MessageSerializerAnalyze.ExistingMessageTypes_Unchanged
// Verifies that all v0.1 message types (0x01–0x06) still produce correctly
// framed and readable messages after the enum extension.
// ---------------------------------------------------------------------------
TEST(MessageSerializerAnalyze, ExistingMessageTypes_Unchanged)
{
    using MT = MessageSerializer::MessageType;

    const std::vector<std::pair<MT, uint8_t>> existingTypes = {
        {MT::REGISTER, 0x01},
        {MT::ACK, 0x02},
        {MT::ERROR_MSG, 0x03},
        {MT::QUERY_NODE, 0x04},
        {MT::LIST_NODES, 0x05},
        {MT::HEARTBEAT, 0x06},
    };

    for (const auto& [type, expectedByte] : existingTypes)
    {
        std::array<int, 2> sv = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()), 0);

        constexpr uint32_t MSG_ID = 1U;
        const std::string payload = R"({"check":"ok"})";
        const auto msgType = type;

        std::thread writer([&]() { sendFrameAsync(sv[1], msgType, MSG_ID, payload); });

        const auto result = MessageSerializer::readFrame(sv[0]);
        writer.join();

        EXPECT_EQ(result.status, MessageSerializer::FrameReadStatus::OK);
        EXPECT_EQ(result.messageType, type);
        EXPECT_EQ(static_cast<uint8_t>(result.messageType), expectedByte);
        EXPECT_EQ(result.messageId, MSG_ID);
        EXPECT_EQ(result.payload, payload);

        ::close(sv[0]);
        ::close(sv[1]);
    }
}
