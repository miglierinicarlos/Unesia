#include "connectionWorker.hpp"
#include "logger.hpp"
#include "nodeRegistry.hpp"
#include "serverConfig.hpp"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <filesystem>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <iostream>
#include <sstream>
#include <thread>

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
    auto [workerFd, peerFd] = makeSocketPair();
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
    auto [workerFd, peerFd] = makeSocketPair();
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
    auto [workerFd, peerFd] = makeSocketPair();
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
        auto [workerFd, peerFd] = makeSocketPair();
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
    auto [workerFd, peerFd] = makeSocketPair();
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
    auto [workerFd, peerFd] = makeSocketPair();
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

TEST_F(connectionWorkerTest, abruptDisconnectCleansUpAllResources)
{
    // Capture baseline FDs
    const int fdsBefore = countOpenFds();
    ASSERT_GE(fdsBefore, 0);

    auto [workerFd, peerFd] = makeSocketPair();
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
