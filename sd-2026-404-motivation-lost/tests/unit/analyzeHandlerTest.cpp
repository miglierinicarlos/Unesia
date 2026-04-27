#include "analyzeHandler.hpp"
#include "logger.hpp"
#include "messageSerializer.hpp"
#include "nodeRegistry.hpp"
#include "serverConfig.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
    constexpr const char* TEST_ENGINE_NODE_ID = "hpc-engine-test";
    constexpr const char* TEST_SESSION_ID = "sess-test-01";
    constexpr uint32_t TEST_MSG_ID = 99U;
    constexpr uint32_t HPC_TIMEOUT_SECS = 1U;

    /* Minimal NodeEntry for the HPC engine */
    NodeRegistry::NodeEntry makeEngineEntry()
    {
        NodeRegistry::NodeEntry e;
        e.node_id = TEST_ENGINE_NODE_ID;
        e.bunker_name = "HPC-Bunker";
        e.ip_address = "127.0.0.1";
        e.capacity = 0;
        return e;
    }

    /* ServerConfig wired for tests: short timeout, known node ID */
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

    /* Returns the error_code field from a parsed ERROR frame payload, or -1 on parse error */
    int extractErrorCode(const std::vector<uint8_t>& frame)
    {
        /* Frame layout: 4B length prefix + 10B envelope + payload */
        constexpr std::size_t HEADER_SIZE =
            MessageSerializer::FRAME_LENGTH_PREFIX_SIZE + MessageSerializer::ENVELOPE_SIZE;
        if (frame.size() <= HEADER_SIZE)
        {
            return -1;
        }
        const std::string payload(frame.begin() + static_cast<std::ptrdiff_t>(HEADER_SIZE), frame.end());
        try
        {
            const auto j = nlohmann::json::parse(payload);
            return j.value("error_code", -1);
        }
        catch (...)
        {
            return -1;
        }
    }

    /* Returns the message_type byte from a raw frame buffer */
    uint8_t extractMessageType(const std::vector<uint8_t>& frame)
    {
        /* Layout: [4B frame_length][1B version][1B msg_type]... */
        constexpr std::size_t MSG_TYPE_OFFSET = MessageSerializer::FRAME_LENGTH_PREFIX_SIZE + 1U;
        if (frame.size() <= MSG_TYPE_OFFSET)
        {
            return 0xFF;
        }
        return frame[MSG_TYPE_OFFSET];
    }
} // namespace

// =============================================================================
// AnalyzeHandler.EngineOffline_ReturnsServiceUnavail
// When the HPC engine node is absent from the registry the handler must send
// an ERROR frame with ERR_SERVICE_UNAVAIL (0x04) to the originating client.
// =============================================================================
TEST(AnalyzeHandler, engineOfflineReturnsServiceUnavail)
{
    NodeRegistry registry;
    const ServerConfig config = makeTestConfig();
    Logger logger(Logger::Level::NONE);

    AnalyzeHandler handler(registry, config, logger);

    std::vector<uint8_t> captured;
    const AnalyzeHandler::SendFn sender = [&](const uint8_t* data, std::size_t size) -> bool
    {
        captured.assign(data, data + size);
        return true;
    };

    handler.handle(TEST_MSG_ID, R"({"algorithm":"dijkstra","graph":{}})", TEST_SESSION_ID, sender);

    ASSERT_FALSE(captured.empty());
    EXPECT_EQ(extractMessageType(captured), static_cast<uint8_t>(MessageSerializer::MessageType::ERROR_MSG));
    EXPECT_EQ(extractErrorCode(captured), static_cast<int>(MessageSerializer::ProtocolErrorCode::ERR_SERVICE_UNAVAIL));
}

// =============================================================================
// AnalyzeHandler.EngineOnlineButOfflineStatus_ReturnsServiceUnavail
// When the engine is registered but marked OFFLINE the handler must immediately
// return ERR_SERVICE_UNAVAIL without attempting any I/O.
// =============================================================================
TEST(AnalyzeHandler, engineOnlineButOfflineStatusReturnsServiceUnavail)
{
    NodeRegistry registry;
    registry.registerNode(makeEngineEntry());
    registry.setOffline(TEST_ENGINE_NODE_ID);

    const ServerConfig config = makeTestConfig();
    Logger logger(Logger::Level::NONE);
    AnalyzeHandler handler(registry, config, logger);

    std::vector<uint8_t> captured;
    const AnalyzeHandler::SendFn sender = [&](const uint8_t* data, std::size_t size) -> bool
    {
        captured.assign(data, data + size);
        return true;
    };

    handler.handle(TEST_MSG_ID, R"({"algorithm":"dijkstra","graph":{}})", TEST_SESSION_ID, sender);

    ASSERT_FALSE(captured.empty());
    EXPECT_EQ(extractMessageType(captured), static_cast<uint8_t>(MessageSerializer::MessageType::ERROR_MSG));
    EXPECT_EQ(extractErrorCode(captured), static_cast<int>(MessageSerializer::ProtocolErrorCode::ERR_SERVICE_UNAVAIL));
}

// =============================================================================
// AnalyzeHandler.EngineTimeout_ReturnsTimeout
// When the engine fd is valid but the engine never writes a response, the
// handler must return ERR_TIMEOUT (0x05) after EOP_HPC_TIMEOUT seconds.
// =============================================================================
TEST(AnalyzeHandler, engineTimeoutReturnsTimeout)
{
    NodeRegistry registry;
    registry.registerNode(makeEngineEntry());

    /* Create a socket pair: clientFd (stored in registry) / serverFd (engine side — never writes) */
    std::array<int, 2> sv = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()), 0);

    registry.setNodeFd(TEST_ENGINE_NODE_ID, sv[0]);

    const ServerConfig config = makeTestConfig(); /* m_hpcTimeoutSecs = 1 */
    Logger logger(Logger::Level::NONE);
    AnalyzeHandler handler(registry, config, logger);

    std::vector<uint8_t> captured;
    const AnalyzeHandler::SendFn sender = [&](const uint8_t* data, std::size_t size) -> bool
    {
        captured.assign(data, data + size);
        return true;
    };

    /* sv[1] is the engine side — we deliberately never write to it, causing a timeout */
    handler.handle(TEST_MSG_ID, R"({"algorithm":"dijkstra","graph":{}})", TEST_SESSION_ID, sender);

    ::close(sv[0]);
    ::close(sv[1]);

    ASSERT_FALSE(captured.empty());
    EXPECT_EQ(extractMessageType(captured), static_cast<uint8_t>(MessageSerializer::MessageType::ERROR_MSG));
    EXPECT_EQ(extractErrorCode(captured), static_cast<int>(MessageSerializer::ProtocolErrorCode::ERR_TIMEOUT));
}

// =============================================================================
// AnalyzeHandler.SuccessPath_ForwardsResult
// When the engine responds with a valid ANALYZE_RESULT frame, the handler must
// forward an ANALYZE_RESULT frame to the originating client.
// =============================================================================
TEST(AnalyzeHandler, successPathForwardsResult)
{
    NodeRegistry registry;
    registry.registerNode(makeEngineEntry());

    std::array<int, 2> sv = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()), 0);

    registry.setNodeFd(TEST_ENGINE_NODE_ID, sv[0]);

    const ServerConfig config = makeTestConfig();
    Logger logger(Logger::Level::NONE);
    AnalyzeHandler handler(registry, config, logger);

    /* Engine simulation: reads ANALYZE_GRAPH from sv[1], writes ANALYZE_RESULT back */
    const std::string resultPayload = MessageSerializer::buildAnalyzeResultPayload(
        {.algorithms = {{.algorithm = "dijkstra", .resultJson = R"({"distances":{"1":0}})"}},
         .graphSize = {.nodes = 3U, .edges = 2U},
         .processingTimeMs = 12U,
         .threadCount = 4U});

    std::thread engineThread(
        [&]()
        {
            /* Consume the ANALYZE_GRAPH request (discard it) */
            const auto req = MessageSerializer::readFrame(sv[1]);
            (void)req;

            /* Send back the ANALYZE_RESULT */
            const auto respFrame = MessageSerializer::buildFrame(
                MessageSerializer::MessageType::ANALYZE_RESULT, TEST_MSG_ID, resultPayload);
            static_cast<void>(MessageSerializer::sendAll(sv[1], respFrame.data(), respFrame.size()));
        });

    std::vector<uint8_t> captured;
    const AnalyzeHandler::SendFn sender = [&](const uint8_t* data, std::size_t size) -> bool
    {
        captured.assign(data, data + size);
        return true;
    };

    handler.handle(TEST_MSG_ID, R"({"algorithm":"dijkstra","graph":{}})", TEST_SESSION_ID, sender);

    engineThread.join();
    ::close(sv[0]);
    ::close(sv[1]);

    ASSERT_FALSE(captured.empty());
    EXPECT_EQ(extractMessageType(captured), static_cast<uint8_t>(MessageSerializer::MessageType::ANALYZE_RESULT));

    /* The forwarded payload must contain the engine's result */
    constexpr std::size_t HEADER_SIZE = MessageSerializer::FRAME_LENGTH_PREFIX_SIZE + MessageSerializer::ENVELOPE_SIZE;
    ASSERT_GT(captured.size(), HEADER_SIZE);
    const std::string forwardedPayload(captured.begin() + static_cast<std::ptrdiff_t>(HEADER_SIZE), captured.end());
    const auto parsed = MessageSerializer::parseAnalyzeResultPayload(forwardedPayload);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->algorithms.size(), 1U);
    EXPECT_EQ(parsed->algorithms[0].algorithm, "dijkstra");
    EXPECT_EQ(parsed->graphSize.nodes, 3U);
}

// =============================================================================
// AnalyzeHandler.MalformedJsonPayload_ReturnsMalformed
// When the ANALYZE_GRAPH payload is not valid JSON the handler must respond
// with an ERROR frame carrying ERR_MALFORMED (0x03).
// Covers the parsed.is_discarded() branch (lines 27-43 of analyzeHandler.cpp).
// =============================================================================
TEST(AnalyzeHandler, malformedJsonPayloadReturnsMalformed)
{
    NodeRegistry registry;
    const ServerConfig config = makeTestConfig();
    Logger logger(Logger::Level::NONE);
    AnalyzeHandler handler(registry, config, logger);

    std::vector<uint8_t> captured;
    const AnalyzeHandler::SendFn sender = [&](const uint8_t* data, std::size_t size) -> bool
    {
        captured.assign(data, data + size);
        return true;
    };

    handler.handle(TEST_MSG_ID, "not-valid-json{{{{", TEST_SESSION_ID, sender);

    ASSERT_FALSE(captured.empty());
    EXPECT_EQ(extractMessageType(captured), static_cast<uint8_t>(MessageSerializer::MessageType::ERROR_MSG));
    EXPECT_EQ(extractErrorCode(captured), static_cast<int>(MessageSerializer::ProtocolErrorCode::ERR_MALFORMED));
}

// =============================================================================
// AnalyzeHandler.EngineClosesConnectionMidDispatch_ReturnsServiceUnavail
// When the engine closes its socket after receiving the request (without
// responding), readFrame returns a non-OK, non-TIMEOUT status and the handler
// must return ERR_SERVICE_UNAVAIL (0x04).
// Covers the result.status != OK branch (lines 160-174 of analyzeHandler.cpp).
// =============================================================================
TEST(AnalyzeHandler, engineClosesConnectionMidDispatchReturnsServiceUnavail)
{
    NodeRegistry registry;
    registry.registerNode(makeEngineEntry());

    std::array<int, 2> sv = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()), 0);

    registry.setNodeFd(TEST_ENGINE_NODE_ID, sv[0]);

    const ServerConfig config = makeTestConfig();
    Logger logger(Logger::Level::NONE);
    AnalyzeHandler handler(registry, config, logger);

    /* Engine simulation: consume the request then immediately close — causes EOF on sv[0] */
    std::thread engineThread(
        [&]()
        {
            const auto req = MessageSerializer::readFrame(sv[1]);
            (void)req;
            ::close(sv[1]);
        });

    std::vector<uint8_t> captured;
    const AnalyzeHandler::SendFn sender = [&](const uint8_t* data, std::size_t size) -> bool
    {
        captured.assign(data, data + size);
        return true;
    };

    handler.handle(TEST_MSG_ID, R"({"algorithm":"dijkstra","graph":{}})", TEST_SESSION_ID, sender);

    engineThread.join();
    ::close(sv[0]);

    ASSERT_FALSE(captured.empty());
    EXPECT_EQ(extractMessageType(captured), static_cast<uint8_t>(MessageSerializer::MessageType::ERROR_MSG));
    EXPECT_EQ(extractErrorCode(captured), static_cast<int>(MessageSerializer::ProtocolErrorCode::ERR_SERVICE_UNAVAIL));
}

// =============================================================================
// AnalyzeHandler.EngineRespondsWrongType_ReturnsInternal
// When the engine responds with an unexpected message type (not ANALYZE_RESULT)
// the handler must return ERR_INTERNAL (0xFF).
// Covers the result.messageType != ANALYZE_RESULT branch (lines 176-191).
// =============================================================================
TEST(AnalyzeHandler, engineRespondsWrongTypeReturnsInternal)
{
    NodeRegistry registry;
    registry.registerNode(makeEngineEntry());

    std::array<int, 2> sv = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()), 0);

    registry.setNodeFd(TEST_ENGINE_NODE_ID, sv[0]);

    const ServerConfig config = makeTestConfig();
    Logger logger(Logger::Level::NONE);
    AnalyzeHandler handler(registry, config, logger);

    /* Engine simulation: respond with HEARTBEAT instead of ANALYZE_RESULT */
    std::thread engineThread(
        [&]()
        {
            const auto req = MessageSerializer::readFrame(sv[1]);
            (void)req;
            const auto wrongFrame =
                MessageSerializer::buildFrame(MessageSerializer::MessageType::HEARTBEAT, TEST_MSG_ID, "{}");
            static_cast<void>(MessageSerializer::sendAll(sv[1], wrongFrame.data(), wrongFrame.size()));
        });

    std::vector<uint8_t> captured;
    const AnalyzeHandler::SendFn sender = [&](const uint8_t* data, std::size_t size) -> bool
    {
        captured.assign(data, data + size);
        return true;
    };

    handler.handle(TEST_MSG_ID, R"({"algorithm":"dijkstra","graph":{}})", TEST_SESSION_ID, sender);

    engineThread.join();
    ::close(sv[0]);
    ::close(sv[1]);

    ASSERT_FALSE(captured.empty());
    EXPECT_EQ(extractMessageType(captured), static_cast<uint8_t>(MessageSerializer::MessageType::ERROR_MSG));
    EXPECT_EQ(extractErrorCode(captured), static_cast<int>(MessageSerializer::ProtocolErrorCode::ERR_INTERNAL));
}

// =============================================================================
// AnalyzeHandler.ConcurrentDispatch_NoDataRace
// Two threads dispatch ANALYZE_GRAPH simultaneously to the same engine.
// The per-node fd mutex must serialize them without data races (TSan clean).
// Each thread gets a valid ANALYZE_RESULT forwarded back.
// =============================================================================
TEST(AnalyzeHandler, concurrentDispatchIsDataRaceFree)
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
        {.algorithms = {{.algorithm = "centrality", .resultJson = R"({"scores":{}})"}},
         .graphSize = {.nodes = 5U, .edges = 4U},
         .processingTimeMs = 8U,
         .threadCount = 2U});

    /* Engine simulation: handles exactly two sequential request/response cycles */
    std::thread engineThread(
        [&]()
        {
            for (int i = 0; i < 2; ++i)
            {
                const auto req = MessageSerializer::readFrame(sv[1]);
                if (req.status != MessageSerializer::FrameReadStatus::OK)
                {
                    break;
                }
                const auto respFrame = MessageSerializer::buildFrame(
                    MessageSerializer::MessageType::ANALYZE_RESULT, req.messageId, resultPayload);
                static_cast<void>(MessageSerializer::sendAll(sv[1], respFrame.data(), respFrame.size()));
            }
        });

    std::atomic<int> successCount {0};

    const auto clientTask = [&]()
    {
        std::vector<uint8_t> captured;
        const AnalyzeHandler::SendFn sender = [&](const uint8_t* data, std::size_t size) -> bool
        {
            captured.assign(data, data + size);
            return true;
        };

        handler.handle(TEST_MSG_ID, R"({"algorithm":"centrality","graph":{}})", TEST_SESSION_ID, sender);

        if (!captured.empty() &&
            extractMessageType(captured) == static_cast<uint8_t>(MessageSerializer::MessageType::ANALYZE_RESULT))
        {
            successCount.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::thread t1(clientTask);
    std::thread t2(clientTask);
    t1.join();
    t2.join();
    engineThread.join();

    ::close(sv[0]);
    ::close(sv[1]);

    /* Both dispatches must have succeeded and been serialized by the fd mutex */
    EXPECT_EQ(successCount.load(), 2);
}
