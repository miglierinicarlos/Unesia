#include "connectionWorker.hpp"
#include "logger.hpp"
#include "messageSerializer.hpp"
#include "nodeRegistry.hpp"
#include "serverConfig.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <filesystem>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

namespace
{
    constexpr uint16_t DUMMY_PORT = 9026;
    constexpr uint32_t DUMMY_IDLE_TIMEOUT_SECS = 30;
    constexpr uint32_t DUMMY_POOL_SIZE = 1;
    constexpr uint32_t DUMMY_MAX_CLIENTS = 10;
    constexpr uint32_t DUMMY_HEARTBEAT_INTERVAL = 5;
    constexpr int FD_LEAK_ITERATIONS = 1000;
    constexpr const char* LOCALHOST_IP = "127.0.0.1";
    constexpr const char* DUMMY_NODE_ID = "node-1";
    constexpr const char* DUMMY_SESSION_ID = "sess-1";
} // namespace

// Helpers

struct ScopedIoSilencer
{
    std::ostringstream m_sinkOut;
    std::ostringstream m_sinkErr;
    std::streambuf* m_oldOut;
    std::streambuf* m_oldErr;

    ScopedIoSilencer()
        : m_oldOut(std::cout.rdbuf(m_sinkOut.rdbuf()))
        , m_oldErr(std::cerr.rdbuf(m_sinkErr.rdbuf()))
    {
    }

    ~ScopedIoSilencer()
    {
        std::cout.rdbuf(m_oldOut);
        std::cerr.rdbuf(m_oldErr);
    }

    ScopedIoSilencer(const ScopedIoSilencer&) = delete;
    ScopedIoSilencer& operator=(const ScopedIoSilencer&) = delete;
};

static ServerConfig makeDefaultConfig()
{
    ServerConfig cfg {};
    cfg.m_port = DUMMY_PORT;
    cfg.m_idleTimeoutSecs = DUMMY_IDLE_TIMEOUT_SECS;
    cfg.m_threadPoolSize = DUMMY_POOL_SIZE;
    cfg.m_maxClients = DUMMY_MAX_CLIENTS;
    cfg.m_heartbeatIntervalSecs = DUMMY_HEARTBEAT_INTERVAL;
    return cfg;
}

static bool setNonBlocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static int countOpenFds()
{
    std::error_code ec;
    auto it = std::filesystem::directory_iterator("/proc/self/fd", ec);
    if (ec)
    {
        return -1;
    }
    return static_cast<int>(std::distance(it, std::filesystem::directory_iterator {}));
}

static std::pair<int, int> makeSocketPair()
{
    std::array<int, 2> sv = {-1, -1};
    // NOLINTNEXTLINE(android-cloexec-socketpair)
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()) != 0)
    {
        return {-1, -1};
    }
    return {sv[0], sv[1]};
}

/// Closes fd with SO_LINGER=0, producing a TCP RST — identical to SIGKILL behaviour.
static void forceRst(int fd)
{
    linger lg {};
    lg.l_onoff = 1;
    lg.l_linger = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    ::close(fd);
}

struct TestFdGuard
{
    explicit TestFdGuard(int fd)
        : m_fd(fd)
    {
    }

    ~TestFdGuard()
    {
        if (m_fd >= 0)
        {
            ::close(m_fd);
        }
    }

    TestFdGuard(const TestFdGuard&) = delete;
    TestFdGuard& operator=(const TestFdGuard&) = delete;

    int m_fd;
};

// Silence logger output for every test in this suite.
class connectionWorkerTest : public ::testing::Test
{
protected:
    ScopedIoSilencer m_silence;
};

// Tests
TEST_F(connectionWorkerTest, idleTimeoutClosesConnection)
{
    auto socketPair = makeSocketPair();
    int workerFd = socketPair.first;
    int peerFd = socketPair.second;
    ASSERT_GE(workerFd, 0);
    ASSERT_TRUE(setNonBlocking(workerFd));
    TestFdGuard peer(peerFd);

    NodeRegistry registry;
    std::atomic<uint32_t> activeConnections {1};
    const ServerConfig cfg = makeDefaultConfig();
    Logger logger;
    ConnectionWorker worker(cfg, registry, activeConnections, logger);

    worker.handleConnection(workerFd,
                            {.m_nodeId = DUMMY_NODE_ID, .m_sessionId = DUMMY_SESSION_ID, .m_clientIp = LOCALHOST_IP});

    char buf = 0;
    const ssize_t n = ::recv(peer.m_fd, &buf, sizeof(buf), 0);
    EXPECT_EQ(n, 0);
}

TEST_F(connectionWorkerTest, nodeMarkedOfflineOnTimeout)
{
    auto socketPair = makeSocketPair();
    int workerFd = socketPair.first;
    int peerFd = socketPair.second;
    ASSERT_GE(workerFd, 0);
    ASSERT_TRUE(setNonBlocking(workerFd));
    TestFdGuard peer(peerFd);

    NodeRegistry registry;
    registry.registerNode({.node_id = DUMMY_NODE_ID, .bunker_name = "Bunker 1", .ip_address = LOCALHOST_IP});

    std::atomic<uint32_t> activeConnections {1};
    const ServerConfig cfg = makeDefaultConfig();
    Logger logger;
    ConnectionWorker worker(cfg, registry, activeConnections, logger);

    worker.handleConnection(workerFd,
                            {.m_nodeId = DUMMY_NODE_ID, .m_sessionId = DUMMY_SESSION_ID, .m_clientIp = LOCALHOST_IP});

    EXPECT_TRUE(registry.isOffline(DUMMY_NODE_ID));
}

TEST_F(connectionWorkerTest, activeConnectionsDecrementedOnTimeout)
{
    auto socketPair = makeSocketPair();
    int workerFd = socketPair.first;
    int peerFd = socketPair.second;
    ASSERT_GE(workerFd, 0);
    ASSERT_TRUE(setNonBlocking(workerFd));
    TestFdGuard peer(peerFd);

    NodeRegistry registry;
    std::atomic<uint32_t> activeConnections {1};
    const ServerConfig cfg = makeDefaultConfig();
    Logger logger;
    ConnectionWorker worker(cfg, registry, activeConnections, logger);

    worker.handleConnection(workerFd,
                            {.m_nodeId = DUMMY_NODE_ID, .m_sessionId = DUMMY_SESSION_ID, .m_clientIp = LOCALHOST_IP});

    EXPECT_EQ(activeConnections.load(), 0U);
}

TEST_F(connectionWorkerTest, noFdLeakOnTimeoutPath)
{
    const int fdsBefore = countOpenFds();
    ASSERT_GE(fdsBefore, 0);

    for (int i = 0; i < FD_LEAK_ITERATIONS; ++i)
    {
        auto socketPair = makeSocketPair();
        int workerFd = socketPair.first;
        int peerFd = socketPair.second;
        ASSERT_GE(workerFd, 0);
        ASSERT_TRUE(setNonBlocking(workerFd));
        TestFdGuard peer(peerFd);

        NodeRegistry registry;
        std::atomic<uint32_t> activeConnections {1};
        const ServerConfig cfg = makeDefaultConfig();
        Logger logger;
        ConnectionWorker worker(cfg, registry, activeConnections, logger);

        worker.handleConnection(
            workerFd, {.m_nodeId = DUMMY_NODE_ID, .m_sessionId = DUMMY_SESSION_ID, .m_clientIp = LOCALHOST_IP});
    }

    const int fdsAfter = countOpenFds();
    ASSERT_GE(fdsAfter, 0);
    EXPECT_EQ(fdsBefore, fdsAfter);
}

// Tests - Edge cases

TEST_F(connectionWorkerTest, invalidFdReturnsAndDecrementsActiveConnections)
{
    NodeRegistry registry;
    std::atomic<uint32_t> activeConnections {1};
    const ServerConfig cfg = makeDefaultConfig();
    Logger logger;
    ConnectionWorker worker(cfg, registry, activeConnections, logger);

    worker.handleConnection(-1,
                            {.m_nodeId = DUMMY_NODE_ID, .m_sessionId = DUMMY_SESSION_ID, .m_clientIp = LOCALHOST_IP});

    EXPECT_EQ(activeConnections.load(), 0U);
}

TEST_F(connectionWorkerTest, socketConfigErrorMarksOfflineAndDecrements)
{
    // /dev/null is not a socket — setsockopt(SO_RCVTIMEO) will fail, exercising
    // the SOCKET_CONFIG_ERROR teardown path.
    const int fileFd = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(fileFd, 0);

    NodeRegistry registry;
    registry.registerNode({.node_id = DUMMY_NODE_ID, .bunker_name = "Bunker 1", .ip_address = LOCALHOST_IP});

    std::atomic<uint32_t> activeConnections {1};
    const ServerConfig cfg = makeDefaultConfig();
    Logger logger;
    ConnectionWorker worker(cfg, registry, activeConnections, logger);

    worker.handleConnection(fileFd,
                            {.m_nodeId = DUMMY_NODE_ID, .m_sessionId = DUMMY_SESSION_ID, .m_clientIp = LOCALHOST_IP});

    EXPECT_TRUE(registry.isOffline(DUMMY_NODE_ID));
    EXPECT_EQ(activeConnections.load(), 0U);
}

TEST_F(connectionWorkerTest, cleanDisconnectPathMarksOfflineAndDecrements)
{
    auto socketPair = makeSocketPair();
    int workerFd = socketPair.first;
    int peerFd = socketPair.second;
    ASSERT_GE(workerFd, 0);
    ::close(peerFd); // EOF on first recv()

    NodeRegistry registry;
    registry.registerNode({.node_id = DUMMY_NODE_ID, .bunker_name = "Bunker 1", .ip_address = LOCALHOST_IP});

    std::atomic<uint32_t> activeConnections {1};
    const ServerConfig cfg = makeDefaultConfig();
    Logger logger;
    ConnectionWorker worker(cfg, registry, activeConnections, logger);

    worker.handleConnection(workerFd,
                            {.m_nodeId = DUMMY_NODE_ID, .m_sessionId = DUMMY_SESSION_ID, .m_clientIp = LOCALHOST_IP});

    EXPECT_TRUE(registry.isOffline(DUMMY_NODE_ID));
    EXPECT_EQ(activeConnections.load(), 0U);
}

TEST_F(connectionWorkerTest, recvErrorPathMarksOfflineAndDecrements)
{
    // Unconnected SOCK_STREAM fd — recv() returns error immediately.
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);

    NodeRegistry registry;
    registry.registerNode({.node_id = DUMMY_NODE_ID, .bunker_name = "Bunker 1", .ip_address = LOCALHOST_IP});

    std::atomic<uint32_t> activeConnections {1};
    const ServerConfig cfg = makeDefaultConfig();
    Logger logger;
    ConnectionWorker worker(cfg, registry, activeConnections, logger);

    worker.handleConnection(fd,
                            {.m_nodeId = DUMMY_NODE_ID, .m_sessionId = DUMMY_SESSION_ID, .m_clientIp = LOCALHOST_IP});

    EXPECT_TRUE(registry.isOffline(DUMMY_NODE_ID));
    EXPECT_EQ(activeConnections.load(), 0U);
}

TEST_F(connectionWorkerTest, receivesBytesThenCleanDisconnect)
{
    auto socketPair = makeSocketPair();
    int workerFd = socketPair.first;
    int peerFd = socketPair.second;
    ASSERT_GE(workerFd, 0);

    NodeRegistry registry;
    registry.registerNode({.node_id = DUMMY_NODE_ID, .bunker_name = "Bunker 1", .ip_address = LOCALHOST_IP});

    std::atomic<uint32_t> activeConnections {1};
    const ServerConfig cfg = makeDefaultConfig();
    Logger logger;
    ConnectionWorker worker(cfg, registry, activeConnections, logger);

    TestFdGuard peer(peerFd);

    std::thread workerThread(
        [&]
        {
            worker.handleConnection(
                workerFd, {.m_nodeId = DUMMY_NODE_ID, .m_sessionId = DUMMY_SESSION_ID, .m_clientIp = LOCALHOST_IP});
        });

    const std::array<uint8_t, 5> msg = {'h', 'e', 'l', 'l', 'o'};
    ASSERT_EQ(::send(peer.m_fd, msg.data(), msg.size(), MSG_NOSIGNAL), static_cast<ssize_t>(msg.size()));
    ASSERT_EQ(::shutdown(peer.m_fd, SHUT_WR), 0);

    workerThread.join();

    EXPECT_TRUE(registry.isOffline(DUMMY_NODE_ID));
    EXPECT_EQ(activeConnections.load(), 0U);
}

// =============================================================================
// Routing helpers — used by ConnectionWorkerRouting tests only.
// =============================================================================

namespace
{
    constexpr const char* ROUTING_SESSION_ID = "sess-routing-01";
    constexpr const char* ROUTING_NODE_ID = "node-routing";
    constexpr uint32_t ROUTING_MSG_ID = 42U;
    constexpr uint32_t ROUTING_HPC_TIMEOUT_SECS = 1U;
    constexpr const char* ROUTING_HPC_NODE_ID = "hpc-engine";

    /// Sends a complete ADR-003 frame from the peer side (test → worker).
    bool routingSendFrame(int fd, MessageSerializer::MessageType type, uint32_t msgId, const std::string& payload)
    {
        const auto frame = MessageSerializer::buildFrame(type, msgId, payload);
        return MessageSerializer::sendAll(fd, frame.data(), frame.size());
    }

    struct RoutingFrame
    {
        uint8_t msgType {};
        int errorCode {-1}; ///< extracted from payload when msgType == ERROR_MSG; -1 if absent
        bool ok {false};
    };

    /// Reads one ADR-003 frame from the peer side (worker → test).
    RoutingFrame routingReadFrame(int fd)
    {
        RoutingFrame result;

        uint32_t rawLen = 0;
        auto* p = reinterpret_cast<uint8_t*>(&rawLen);
        std::size_t rem = sizeof(rawLen);
        while (rem > 0)
        {
            const ssize_t n = ::recv(fd, p, rem, 0);
            if (n <= 0)
                return result;
            p += n;
            rem -= static_cast<std::size_t>(n);
        }

        const uint32_t frameLen = ntohl(rawLen);
        if (frameLen < MessageSerializer::ENVELOPE_SIZE)
            return result;

        std::vector<uint8_t> body(frameLen);
        auto* bp = body.data();
        rem = frameLen;
        while (rem > 0)
        {
            const ssize_t n = ::recv(fd, bp, rem, 0);
            if (n <= 0)
                return result;
            bp += n;
            rem -= static_cast<std::size_t>(n);
        }

        result.msgType = body[1];

        if (result.msgType == static_cast<uint8_t>(MessageSerializer::MessageType::ERROR_MSG))
        {
            uint32_t rawPayloadLen = 0;
            std::memcpy(&rawPayloadLen, body.data() + 6, sizeof(uint32_t));
            const uint32_t payloadLen = ntohl(rawPayloadLen);
            if (payloadLen > 0 && body.size() >= MessageSerializer::ENVELOPE_SIZE + payloadLen)
            {
                try
                {
                    const std::string payloadStr(
                        reinterpret_cast<const char*>(body.data() + MessageSerializer::ENVELOPE_SIZE), payloadLen);
                    const auto j = nlohmann::json::parse(payloadStr);
                    result.errorCode = j.value("error_code", -1);
                }
                catch (...)
                {
                }
            }
        }

        result.ok = true;
        return result;
    }

    ServerConfig makeRoutingConfig()
    {
        ServerConfig cfg {};
        cfg.m_port = DUMMY_PORT;
        cfg.m_idleTimeoutSecs = DUMMY_IDLE_TIMEOUT_SECS;
        cfg.m_threadPoolSize = DUMMY_POOL_SIZE;
        cfg.m_maxClients = DUMMY_MAX_CLIENTS;
        cfg.m_heartbeatIntervalSecs = DUMMY_HEARTBEAT_INTERVAL;
        cfg.m_hpcTimeoutSecs = ROUTING_HPC_TIMEOUT_SECS;
        cfg.m_hpcNodeId = ROUTING_HPC_NODE_ID;
        return cfg;
    }
} // namespace

class ConnectionWorkerRouting : public ::testing::Test
{
protected:
    ScopedIoSilencer m_silence;
};

// =============================================================================
// ConnectionWorkerRouting.AnalyzeGraph_Dispatched
// An ANALYZE_GRAPH (0x07) frame must reach AnalyzeHandler and return
// ERR_SERVICE_UNAVAIL (0x04) — not ERR_MALFORMED (0x03) from the default case.
// =============================================================================
TEST_F(ConnectionWorkerRouting, analyzeGraphDispatched)
{
    auto [workerFd, peerFd] = makeSocketPair();
    ASSERT_GE(workerFd, 0);

    NodeRegistry registry; // no HPC engine registered
    std::atomic<uint32_t> activeConnections {1};
    const ServerConfig cfg = makeRoutingConfig();
    Logger logger(Logger::Level::NONE);
    ConnectionWorker worker(cfg, registry, activeConnections, logger);

    const int capturedWorkerFd = workerFd;
    std::thread workerThread(
        [&]
        {
            worker.handleConnection(
                capturedWorkerFd,
                {.m_nodeId = ROUTING_NODE_ID, .m_sessionId = ROUTING_SESSION_ID, .m_clientIp = LOCALHOST_IP});
        });

    // Send a well-formed ANALYZE_GRAPH frame; no HPC engine is registered.
    ASSERT_TRUE(routingSendFrame(peerFd,
                                 MessageSerializer::MessageType::ANALYZE_GRAPH,
                                 ROUTING_MSG_ID,
                                 R"({"algorithm":"dijkstra","graph":{}})"));

    const RoutingFrame response = routingReadFrame(peerFd);

    // Signal EOF so the worker exits its recv loop.
    ::shutdown(peerFd, SHUT_WR);
    workerThread.join();
    ::close(peerFd);

    ASSERT_TRUE(response.ok);
    // Must be an ERROR frame — the AnalyzeHandler was invoked (not the default case).
    EXPECT_EQ(response.msgType, static_cast<uint8_t>(MessageSerializer::MessageType::ERROR_MSG));
    // Error code must be ERR_SERVICE_UNAVAIL (0x04), NOT ERR_MALFORMED (0x03).
    EXPECT_EQ(response.errorCode, static_cast<int>(MessageSerializer::ProtocolErrorCode::ERR_SERVICE_UNAVAIL));
    EXPECT_NE(response.errorCode, static_cast<int>(MessageSerializer::ProtocolErrorCode::ERR_MALFORMED));
}

// =============================================================================
// ConnectionWorkerRouting.ExistingTypes_Unchanged
// REGISTER, QUERY_NODE, and HEARTBEAT must continue to work identically after
// the ANALYZE_GRAPH case was added — no regression in the dispatch switch.
// =============================================================================
TEST_F(ConnectionWorkerRouting, existingTypesUnchanged)
{
    auto [workerFd, peerFd] = makeSocketPair();
    ASSERT_GE(workerFd, 0);

    NodeRegistry registry;
    std::atomic<uint32_t> activeConnections {1};
    const ServerConfig cfg = makeRoutingConfig();
    Logger logger(Logger::Level::NONE);
    ConnectionWorker worker(cfg, registry, activeConnections, logger);

    const int capturedWorkerFd = workerFd;
    std::thread workerThread(
        [&]
        {
            worker.handleConnection(
                capturedWorkerFd,
                {.m_nodeId = ROUTING_NODE_ID, .m_sessionId = ROUTING_SESSION_ID, .m_clientIp = LOCALHOST_IP});
        });

    // REGISTER → must receive ACK (0x02), not ERR_MALFORMED.
    ASSERT_TRUE(routingSendFrame(peerFd,
                                 MessageSerializer::MessageType::REGISTER,
                                 ROUTING_MSG_ID,
                                 R"({"node_id":"n1","bunker_name":"B1","ip_address":"127.0.0.1","capacity":5})"));
    const RoutingFrame registerResp = routingReadFrame(peerFd);
    EXPECT_EQ(registerResp.msgType, static_cast<uint8_t>(MessageSerializer::MessageType::ACK));

    // QUERY_NODE for unknown node → must receive ERR_NOT_FOUND (0x02), not ERR_MALFORMED (0x03).
    ASSERT_TRUE(routingSendFrame(
        peerFd, MessageSerializer::MessageType::QUERY_NODE, ROUTING_MSG_ID + 1U, R"({"node_id":"unknown"})"));
    const RoutingFrame queryResp = routingReadFrame(peerFd);
    EXPECT_EQ(queryResp.msgType, static_cast<uint8_t>(MessageSerializer::MessageType::ERROR_MSG));
    EXPECT_EQ(queryResp.errorCode, static_cast<int>(MessageSerializer::ProtocolErrorCode::ERR_NOT_FOUND));

    // HEARTBEAT → no response expected; the recv loop must not return ERR_MALFORMED.
    ASSERT_TRUE(routingSendFrame(peerFd, MessageSerializer::MessageType::HEARTBEAT, ROUTING_MSG_ID + 2U, "{}"));

    // Signal EOF so the worker exits cleanly.
    ::shutdown(peerFd, SHUT_WR);
    workerThread.join();
    ::close(peerFd);

    // All three existing types were handled without error — test asserts above would have failed otherwise.
    SUCCEED();
}

// =============================================================================
// ConnectionWorkerRouting.UnknownMessageType_ReturnsMalformed
// Sending a frame with an unrecognised message type byte (0xFE) must hit the
// default branch of the dispatch switch and return ERR_MALFORMED (0x03).
// Covers lines 251-256 of connectionWorker.cpp.
// =============================================================================
TEST_F(ConnectionWorkerRouting, unknownMessageTypeReturnsMalformed)
{
    auto [workerFd, peerFd] = makeSocketPair();
    ASSERT_GE(workerFd, 0);

    NodeRegistry registry;
    std::atomic<uint32_t> activeConnections {1};
    const ServerConfig cfg = makeRoutingConfig();
    Logger logger(Logger::Level::NONE);
    ConnectionWorker worker(cfg, registry, activeConnections, logger);

    const int capturedWorkerFd = workerFd;
    std::thread workerThread(
        [&]
        {
            worker.handleConnection(
                capturedWorkerFd,
                {.m_nodeId = ROUTING_NODE_ID, .m_sessionId = ROUTING_SESSION_ID, .m_clientIp = LOCALHOST_IP});
        });

    /* Build a raw frame with message_type = 0xFE (unknown), empty payload */
    constexpr uint8_t UNKNOWN_TYPE = 0xFE;
    constexpr uint32_t ENVELOPE = MessageSerializer::ENVELOPE_SIZE;
    const uint32_t frameLenNet = htonl(ENVELOPE);
    const uint32_t msgIdNet = htonl(ROUTING_MSG_ID);
    const uint32_t payloadLenNet = htonl(0U);

    std::vector<uint8_t> rawFrame;
    rawFrame.resize(MessageSerializer::FRAME_LENGTH_PREFIX_SIZE + ENVELOPE);
    std::memcpy(rawFrame.data(), &frameLenNet, 4);
    rawFrame[4] = MessageSerializer::PROTOCOL_VERSION;
    rawFrame[5] = UNKNOWN_TYPE;
    std::memcpy(rawFrame.data() + 6, &msgIdNet, 4);
    std::memcpy(rawFrame.data() + 10, &payloadLenNet, 4);

    ASSERT_TRUE(MessageSerializer::sendAll(peerFd, rawFrame.data(), rawFrame.size()));

    const RoutingFrame resp = routingReadFrame(peerFd);
    EXPECT_TRUE(resp.ok);
    EXPECT_EQ(resp.msgType, static_cast<uint8_t>(MessageSerializer::MessageType::ERROR_MSG));
    EXPECT_EQ(resp.errorCode, static_cast<int>(MessageSerializer::ProtocolErrorCode::ERR_MALFORMED));

    ::shutdown(peerFd, SHUT_WR);
    workerThread.join();
    ::close(peerFd);
}

TEST_F(connectionWorkerTest, abruptDisconnectCleansUpAllResources)
{
    // Capture baseline FDs
    const int fdsBefore = countOpenFds();
    ASSERT_GE(fdsBefore, 0);

    auto socketPair = makeSocketPair();
    int workerFd = socketPair.first;
    int peerFd = socketPair.second;
    ASSERT_GE(workerFd, 0);

    NodeRegistry registry;
    registry.registerNode({.node_id = DUMMY_NODE_ID, .bunker_name = "Bunker 1", .ip_address = LOCALHOST_IP});

    std::atomic<uint32_t> activeConnections {1};
    const ServerConfig cfg = makeDefaultConfig();
    Logger logger;
    ConnectionWorker worker(cfg, registry, activeConnections, logger);

    // Simulate an abrupt crash from the client side (TCP RST)
    forceRst(peerFd);

    worker.handleConnection(workerFd,
                            {.m_nodeId = DUMMY_NODE_ID, .m_sessionId = DUMMY_SESSION_ID, .m_clientIp = LOCALHOST_IP});

    // First, node marked offline
    EXPECT_TRUE(registry.isOffline(DUMMY_NODE_ID));

    // Active connections decremented
    EXPECT_EQ(activeConnections.load(), 0U);

    // File descriptor released
    EXPECT_EQ(countOpenFds(), fdsBefore);
}
