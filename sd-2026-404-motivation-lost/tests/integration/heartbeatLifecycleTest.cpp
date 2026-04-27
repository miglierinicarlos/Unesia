/**
 * @file heartbeatLifecycleTest.cpp
 * @brief Integration test for US-104 AC7 — full node lifecycle:
 *        register → heartbeat → abrupt disconnect → OFFLINE → reconnect → ONLINE
 */

#include "connectionWorker.hpp"
#include "heartbeatMonitor.hpp"
#include "logger.hpp"
#include "messageSerializer.hpp"
#include "nodeRegistry.hpp"
#include "serverConfig.hpp"
#include "sessionManager.hpp"
#include "socketAcceptor.hpp"
#include "threadPool.hpp"
#include "workQueue.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
    constexpr uint16_t TEST_PORT = 19055;
    constexpr uint32_t TEST_HEARTBEAT_INTERVAL_SECS = 2;
    constexpr uint32_t TEST_IDLE_TIMEOUT_SECS = 60;
    constexpr uint32_t TEST_POOL_SIZE = 8;
    constexpr uint32_t TEST_MAX_CLIENTS = 100;
    constexpr const char* LOCALHOST = "127.0.0.1";

    // After 2 × interval the monitor marks stale nodes OFFLINE.
    // We wait 2 × interval + 1 extra interval as buffer for the monitor tick.
    constexpr int OFFLINE_DETECTION_WAIT_SECS = TEST_HEARTBEAT_INTERVAL_SECS * 3;

    ServerConfig makeConfig()
    {
        ServerConfig cfg {};
        cfg.m_port = TEST_PORT;
        cfg.m_idleTimeoutSecs = TEST_IDLE_TIMEOUT_SECS;
        cfg.m_threadPoolSize = TEST_POOL_SIZE;
        cfg.m_maxClients = TEST_MAX_CLIENTS;
        cfg.m_heartbeatIntervalSecs = TEST_HEARTBEAT_INTERVAL_SECS;
        return cfg;
    }

    int connectToServer()
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            return -1;
        }

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(TEST_PORT);
        addr.sin_addr.s_addr = ::inet_addr(LOCALHOST);

        if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    /// Reads the welcome frame sent by the ThreadPool on new connection.
    bool drainWelcomeFrame(int fd)
    {
        const auto result = MessageSerializer::readFrame(fd);
        return result.status == MessageSerializer::FrameReadStatus::OK;
    }

    /// Sends a REGISTER frame and returns true if an ACK is received.
    bool sendRegister(int fd, const std::string& nodeId)
    {
        nlohmann::json payload;
        payload["node_id"] = nodeId;
        payload["bunker_name"] = "Test Bunker";
        payload["ip_address"] = LOCALHOST;
        payload["capacity"] = 100;

        const auto frame = MessageSerializer::buildFrame(MessageSerializer::MessageType::REGISTER, 1, payload.dump());
        if (!MessageSerializer::sendAll(fd, frame.data(), frame.size()))
        {
            return false;
        }

        const auto result = MessageSerializer::readFrame(fd);
        return result.status == MessageSerializer::FrameReadStatus::OK &&
               result.messageType == MessageSerializer::MessageType::ACK;
    }

    /// Sends a single HEARTBEAT frame (fire-and-forget, no response expected).
    bool sendHeartbeat(int fd)
    {
        const auto frame = MessageSerializer::buildFrame(MessageSerializer::MessageType::HEARTBEAT, 2, "");
        return MessageSerializer::sendAll(fd, frame.data(), frame.size());
    }

    /// Sends a QUERY_NODE and returns the status string from the ACK payload.
    /// Returns empty string on failure.
    std::string queryNodeStatus(int fd, const std::string& nodeId, uint32_t msgId)
    {
        nlohmann::json payload;
        payload["node_id"] = nodeId;

        const auto frame =
            MessageSerializer::buildFrame(MessageSerializer::MessageType::QUERY_NODE, msgId, payload.dump());
        if (!MessageSerializer::sendAll(fd, frame.data(), frame.size()))
        {
            return "";
        }

        const auto result = MessageSerializer::readFrame(fd);
        if (result.status != MessageSerializer::FrameReadStatus::OK ||
            result.messageType != MessageSerializer::MessageType::ACK)
        {
            return "";
        }

        try
        {
            const auto j = nlohmann::json::parse(result.payload);
            return j.value("status", "");
        }
        catch (...)
        {
            return "";
        }
    }

    /// Simulates SIGKILL by closing with SO_LINGER=0 — sends TCP RST.
    void forceRst(int fd)
    {
        linger lg {};
        lg.l_onoff = 1;
        lg.l_linger = 0;
        ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        ::close(fd);
    }
} // namespace

// =============================================================================
// Fixture
// =============================================================================

class HeartbeatLifecycleTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        s_config = std::make_unique<ServerConfig>(makeConfig());
        s_logger = std::make_unique<Logger>(Logger::Level::NONE);
        s_registry = std::make_unique<NodeRegistry>();
        s_sessionManager = std::make_unique<SessionManager>();
        s_workQueue = std::make_unique<WorkQueue>();

        s_worker = std::make_unique<ConnectionWorker>(*s_config, *s_registry, s_activeConnections, *s_logger);

        s_pool =
            std::make_unique<ThreadPool>(*s_config, *s_workQueue, *s_worker, *s_sessionManager, s_activeConnections);

        s_monitor = std::make_unique<HeartbeatMonitor>(*s_registry, *s_logger, *s_config);

        s_acceptor = std::make_unique<SocketAcceptor>(*s_config,
                                                      s_activeConnections,
                                                      *s_logger,
                                                      [](int fd, const std::string& ip)
                                                      {
                                                          s_activeConnections.fetch_add(1, std::memory_order_relaxed);
                                                          s_workQueue->enqueue({fd, ip});
                                                      });

        ASSERT_TRUE(s_acceptor->start());
        s_monitor->start();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    static void TearDownTestSuite()
    {
        s_monitor->stop();
        s_acceptor->stop();
        s_pool->shutdown();

        s_acceptor.reset();
        s_monitor.reset();
        s_pool.reset();
        s_worker.reset();
        s_workQueue.reset();
        s_sessionManager.reset();
        s_registry.reset();
        s_logger.reset();
        s_config.reset();
    }

    inline static std::unique_ptr<ServerConfig> s_config;
    inline static std::unique_ptr<Logger> s_logger;
    inline static std::unique_ptr<NodeRegistry> s_registry;
    inline static std::unique_ptr<SessionManager> s_sessionManager;
    inline static std::unique_ptr<WorkQueue> s_workQueue;
    inline static std::unique_ptr<ConnectionWorker> s_worker;
    inline static std::unique_ptr<ThreadPool> s_pool;
    inline static std::unique_ptr<HeartbeatMonitor> s_monitor;
    inline static std::unique_ptr<SocketAcceptor> s_acceptor;
    inline static std::atomic<uint32_t> s_activeConnections {0};
};

// =============================================================================
// Tests
// =============================================================================

// =============================================================================
// HeartbeatLifecycleTest.fullDisconnectionAndRecoveryCycle
// Verifies AC7: register → heartbeat → kill → OFFLINE → reconnect → ONLINE
// =============================================================================
TEST_F(HeartbeatLifecycleTest, fullDisconnectionAndRecoveryCycle)
{
    const std::string nodeId = "vault-lifecycle-01";

    // Phase 1: register and send heartbeats for two intervals
    {
        const int fd = connectToServer();
        ASSERT_GE(fd, 0);
        ASSERT_TRUE(drainWelcomeFrame(fd));
        ASSERT_TRUE(sendRegister(fd, nodeId));

        for (int i = 0; i < 2; ++i)
        {
            ASSERT_TRUE(sendHeartbeat(fd));
            std::this_thread::sleep_for(std::chrono::seconds(TEST_HEARTBEAT_INTERVAL_SECS));
        }

        // Phase 2: simulate SIGKILL — abrupt disconnect via TCP RST
        forceRst(fd);
    }

    // Phase 3: wait for the monitor to detect the stale node
    std::this_thread::sleep_for(std::chrono::seconds(OFFLINE_DETECTION_WAIT_SECS));

    // Phase 4: verify OFFLINE via a separate query connection
    {
        const int queryFd = connectToServer();
        ASSERT_GE(queryFd, 0);
        ASSERT_TRUE(drainWelcomeFrame(queryFd));

        const std::string status = queryNodeStatus(queryFd, nodeId, 10);
        EXPECT_EQ(status, "OFFLINE") << "Node should be OFFLINE after missed heartbeats";

        ::close(queryFd);
    }

    // Phase 5: reconnect and re-register with the same nodeId
    {
        const int reconnectFd = connectToServer();
        ASSERT_GE(reconnectFd, 0);
        ASSERT_TRUE(drainWelcomeFrame(reconnectFd));
        ASSERT_TRUE(sendRegister(reconnectFd, nodeId));

        // Phase 6: verify back to ONLINE
        const std::string status = queryNodeStatus(reconnectFd, nodeId, 20);
        EXPECT_EQ(status, "ONLINE") << "Node should be ONLINE after re-registration";

        ::close(reconnectFd);
    }
}

// =============================================================================
// HeartbeatLifecycleTest.offlineDetectedWithinTenSeconds
// Verifies AC1: OFFLINE transition detected within 10 s
// =============================================================================
TEST_F(HeartbeatLifecycleTest, offlineDetectedWithinTenSeconds)
{
    const std::string nodeId = "vault-timing-01";

    // Register and immediately kill — no heartbeats sent
    {
        const int fd = connectToServer();
        ASSERT_GE(fd, 0);
        ASSERT_TRUE(drainWelcomeFrame(fd));
        ASSERT_TRUE(sendRegister(fd, nodeId));
        forceRst(fd);
    }

    // Poll for OFFLINE status, checking every 500 ms up to 10 s
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    bool detectedOffline = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        const int queryFd = connectToServer();
        if (queryFd < 0)
        {
            continue;
        }

        drainWelcomeFrame(queryFd);
        const std::string status = queryNodeStatus(queryFd, nodeId, 30);
        ::close(queryFd);

        if (status == "OFFLINE")
        {
            detectedOffline = true;
            break;
        }
    }

    EXPECT_TRUE(detectedOffline) << "Node was not marked OFFLINE within 10 seconds (AC1)";
}

// =============================================================================
// HeartbeatLifecycleTest.monitorDetectsStaleNodeWithOpenConnection
// Verifies AC1 via the heartbeat monitor path — TCP connection stays open
// but heartbeats stop arriving. Detection must occur within 10 s.
// =============================================================================
TEST_F(HeartbeatLifecycleTest, monitorDetectsStaleNodeWithOpenConnection)
{
    const std::string nodeId = "vault-stale-01";

    const int fd = connectToServer();
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(drainWelcomeFrame(fd));
    ASSERT_TRUE(sendRegister(fd, nodeId));

    // Send one heartbeat to establish last_seen_at, then stop
    ASSERT_TRUE(sendHeartbeat(fd));

    // Keep TCP connection open but send no more heartbeats.
    // The monitor must mark the node OFFLINE after 2 × interval.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool detectedOffline = false;

    while (std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        const int queryFd = connectToServer();
        if (queryFd < 0)
        {
            continue;
        }

        drainWelcomeFrame(queryFd);
        const std::string status = queryNodeStatus(queryFd, nodeId, 50);
        ::close(queryFd);

        if (status == "OFFLINE")
        {
            detectedOffline = true;
            break;
        }
    }

    // Clean up the stale connection
    ::close(fd);

    EXPECT_TRUE(detectedOffline) << "Monitor did not detect stale node within 10 s with TCP connection kept open (AC1)";
}

// =============================================================================
// HeartbeatLifecycleTest.reregistrationRestoresOnlineState
// Verifies AC5: re-registration of OFFLINE node returns ONLINE
// =============================================================================
TEST_F(HeartbeatLifecycleTest, reregistrationRestoresOnlineState)
{
    const std::string nodeId = "vault-reregister-01";

    // Register then immediately go offline
    {
        const int fd = connectToServer();
        ASSERT_GE(fd, 0);
        ASSERT_TRUE(drainWelcomeFrame(fd));
        ASSERT_TRUE(sendRegister(fd, nodeId));
        forceRst(fd);
    }

    // Wait for OFFLINE detection
    std::this_thread::sleep_for(std::chrono::seconds(OFFLINE_DETECTION_WAIT_SECS));

    // Re-register — must succeed (not ERR_DUPLICATE)
    {
        const int fd = connectToServer();
        ASSERT_GE(fd, 0);
        ASSERT_TRUE(drainWelcomeFrame(fd));
        ASSERT_TRUE(sendRegister(fd, nodeId)) << "Re-registration of OFFLINE node must return ACK, not ERR_DUPLICATE";

        const std::string status = queryNodeStatus(fd, nodeId, 40);
        EXPECT_EQ(status, "ONLINE");

        ::close(fd);
    }
}
