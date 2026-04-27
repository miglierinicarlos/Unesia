#include "analyzeHandler.hpp"
#include "hpc/hpcHandler.hpp"
#include "logger.hpp"
#include "messageSerializer.hpp"
#include "nodeRegistry.hpp"
#include "otelScope.hpp"
#include "serverConfig.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
    constexpr const char* TEST_ENGINE_NODE_ID = "hpc-engine-test";
    constexpr const char* TEST_SESSION_ID = "sess-otel-01";
    constexpr uint32_t TEST_MSG_ID = 42U;
    constexpr uint32_t HPC_TIMEOUT_SECS = 2U;

    NodeRegistry::NodeEntry makeEngineEntry()
    {
        NodeRegistry::NodeEntry e;
        e.node_id = TEST_ENGINE_NODE_ID;
        e.bunker_name = "HPC-OTel-Test";
        e.ip_address = "127.0.0.1";
        e.capacity = 0;
        return e;
    }

    ServerConfig makeTestConfig()
    {
        ServerConfig cfg {};
        cfg.m_port = 9026;
        cfg.m_idleTimeoutSecs = 30;
        cfg.m_threadPoolSize = 4;
        cfg.m_maxClients = 100;
        cfg.m_heartbeatIntervalSecs = 5;
        cfg.m_hpcTimeoutSecs = HPC_TIMEOUT_SECS;
        cfg.m_hpcNodeId = TEST_ENGINE_NODE_ID;
        return cfg;
    }

    uint8_t extractMessageType(const std::vector<uint8_t>& frame)
    {
        constexpr std::size_t MSG_TYPE_OFFSET = MessageSerializer::FRAME_LENGTH_PREFIX_SIZE + 1U;
        if (frame.size() <= MSG_TYPE_OFFSET)
            return 0xFF;
        return frame[MSG_TYPE_OFFSET];
    }
} // namespace

// =============================================================================
// OtelTracePropagation.OtelScope_ParentLinkage
// OtelScope constructed with a known external trace_id must reuse that ID
// instead of generating a fresh one, while still generating a unique span_id.
// =============================================================================
TEST(OtelTracePropagation, OtelScope_ParentLinkage)
{
    const std::string knownTraceId = "aabbccdd11223344aabbccdd11223344";

    OtelScope scope("test_op", OtelTraceContext {knownTraceId});

    EXPECT_EQ(scope.traceId(), knownTraceId);
    EXPECT_FALSE(scope.spanId().empty());
    EXPECT_NE(scope.spanId(), knownTraceId);
}

// =============================================================================
// OtelTracePropagation.OtelScope_EmptyParent_GeneratesNewTrace
// When parentTraceId is empty the constructor must behave like the single-arg
// constructor: generate a fresh, non-empty trace_id.
// =============================================================================
TEST(OtelTracePropagation, OtelScope_EmptyParent_GeneratesNewTrace)
{
    OtelScope scope("test_op", OtelTraceContext {""});

    EXPECT_FALSE(scope.traceId().empty());
    EXPECT_FALSE(scope.spanId().empty());
}

// =============================================================================
// OtelTracePropagation.DispatchSpan_IncludesAttributes
// AnalyzeHandler must forward the client's trace_id inside the ANALYZE_GRAPH
// payload sent to the engine, and must return an ANALYZE_RESULT to the client.
// =============================================================================
TEST(OtelTracePropagation, DispatchSpan_IncludesAttributes)
{
    NodeRegistry registry;
    registry.registerNode(makeEngineEntry());

    std::array<int, 2> sv = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()), 0);
    registry.setNodeFd(TEST_ENGINE_NODE_ID, sv[0]);

    const ServerConfig config = makeTestConfig();
    Logger logger(Logger::Level::NONE);
    AnalyzeHandler handler(registry, config, logger);

    const std::string resultPayload = MessageSerializer::buildAnalyzeResultPayload(
        {.algorithms = {{.algorithm = "dijkstra", .resultJson = R"({"distances":{"0":0}})"}},
         .graphSize = {.nodes = 1U, .edges = 0U},
         .processingTimeMs = 5U,
         .threadCount = 1U});

    const std::string knownTraceId = "deadbeef01234567deadbeef01234567";
    std::string capturedEnginePayload;

    std::thread engineThread(
        [&]()
        {
            const auto req = MessageSerializer::readFrame(sv[1]);
            capturedEnginePayload = req.payload;

            const auto respFrame = MessageSerializer::buildFrame(
                MessageSerializer::MessageType::ANALYZE_RESULT, req.messageId, resultPayload);
            static_cast<void>(MessageSerializer::sendAll(sv[1], respFrame.data(), respFrame.size()));
        });

    const std::string inputPayload = R"({"trace_id":")" + knownTraceId + R"(","adjacency":[[]]})";

    std::vector<uint8_t> captured;
    const AnalyzeHandler::SendFn sender = [&](const uint8_t* data, std::size_t size) -> bool
    {
        captured.assign(data, data + size);
        return true;
    };

    handler.handle(TEST_MSG_ID, inputPayload, TEST_SESSION_ID, sender);

    engineThread.join();
    ::close(sv[0]);
    ::close(sv[1]);

    /* Engine must have received the trace_id */
    ASSERT_FALSE(capturedEnginePayload.empty());
    const auto engineJson = nlohmann::json::parse(capturedEnginePayload);
    ASSERT_TRUE(engineJson.contains("trace_id"));
    EXPECT_EQ(engineJson["trace_id"].get<std::string>(), knownTraceId);

    /* Client must receive ANALYZE_RESULT (not an error) */
    ASSERT_FALSE(captured.empty());
    EXPECT_EQ(extractMessageType(captured), static_cast<uint8_t>(MessageSerializer::MessageType::ANALYZE_RESULT));
}

// =============================================================================
// OtelTracePropagation.E2E_TraceLinkedThreeSpans
// HpcHandler::processRequest must emit a log line whose trace_id matches the
// parentTraceId passed in, confirming the engine span continues the same trace.
// =============================================================================
TEST(OtelTracePropagation, E2E_TraceLinkedThreeSpans)
{
    const std::string knownTraceId = "cafebabe11223344cafebabe11223344";

    const std::string validPayload = R"({
        "trace_id": ")" + knownTraceId +
                                     R"(",
        "adjacency": [
            [{"destination": 1, "weight": 5}],
            []
        ]
    })";

    testing::internal::CaptureStdout();
    const std::string response = eop::hpc::HpcHandler::processRequest(validPayload, knownTraceId);
    const std::string logOutput = testing::internal::GetCapturedStdout();

    /* Response must be a valid length-prefixed JSON (not an error) */
    ASSERT_GT(response.size(), 4U);

    /* Log line must carry the propagated trace_id */
    ASSERT_NO_THROW({
        const auto logJson = nlohmann::json::parse(logOutput);
        ASSERT_TRUE(logJson.contains("trace_id"));
        EXPECT_EQ(logJson["trace_id"].get<std::string>(), knownTraceId);
        ASSERT_TRUE(logJson.contains("span_id"));
        EXPECT_FALSE(logJson["span_id"].get<std::string>().empty());
    });
}
