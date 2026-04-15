#include "threadPool.hpp"
#include "connectionWorker.hpp"
#include "logger.hpp"
#include "nodeRegistry.hpp"
#include "serverConfig.hpp"
#include "sessionManager.hpp"
#include "workQueue.hpp"

#include <gtest/gtest.h>
#include <sys/socket.h>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>

namespace
{
    constexpr uint16_t DUMMY_PORT = 19027;
    constexpr uint32_t DUMMY_IDLE_TIMEOUT = 1;
    constexpr int INVALID_FD = -1;

    constexpr uint32_t TEST_POOL_SIZE = 4;
    constexpr int TEST_NUM_CONNECTIONS = 50;

    constexpr const char* LOCALHOST_IP = "127.0.0.1";
} // namespace

static int makeFd()
{
    std::array<int, 2> sv = {INVALID_FD, INVALID_FD};
    // NOLINTNEXTLINE(android-cloexec-socketpair)
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()) != 0)
    {
        return INVALID_FD;
    }
    ::close(sv[1]);
    return sv[0];
}

static ServerConfig makeConfig(uint32_t poolSize)
{
    ServerConfig cfg {};
    cfg.m_port = DUMMY_PORT;
    cfg.m_idleTimeoutSecs = DUMMY_IDLE_TIMEOUT;
    cfg.m_threadPoolSize = poolSize;
    cfg.m_maxClients = ServerConfig::DEFAULT_MAX_CLIENTS;
    cfg.m_heartbeatIntervalSecs = ServerConfig::DEFAULT_HEARTBEAT_INTERVAL;
    return cfg;
}

static void enqueueDummyConnection(WorkQueue& wq)
{
    const int fd = makeFd();
    ASSERT_GE(fd, 0);
    wq.enqueue({fd, LOCALHOST_IP});
}

class ThreadPoolTest : public ::testing::Test
{
protected:
    ThreadPoolTest()
        : m_logger(Logger::Level::NONE)
    {
    }

    NodeRegistry m_registry;
    Logger m_logger;
    SessionManager m_sessionManager;
    std::atomic<uint32_t> m_activeConnections {0};
};

TEST_F(ThreadPoolTest, poolStartsCorrectNumberOfThreads)
{
    const ServerConfig cfg = makeConfig(TEST_POOL_SIZE);

    WorkQueue wq;
    ConnectionWorker worker(cfg, m_registry, m_activeConnections, m_logger);
    ThreadPool pool(cfg, wq, worker, m_sessionManager, m_activeConnections);

    EXPECT_EQ(pool.workerCount(), TEST_POOL_SIZE);
    pool.shutdown();
}

TEST_F(ThreadPoolTest, poolDefaultsToConfiguredValue)
{
    const ServerConfig cfg = makeConfig(ServerConfig::DEFAULT_THREAD_POOL_SIZE);

    WorkQueue wq;
    ConnectionWorker worker(cfg, m_registry, m_activeConnections, m_logger);
    ThreadPool pool(cfg, wq, worker, m_sessionManager, m_activeConnections);

    EXPECT_EQ(pool.workerCount(), ServerConfig::DEFAULT_THREAD_POOL_SIZE);
    pool.shutdown();
}

TEST_F(ThreadPoolTest, shutdownIsIdempotent)
{
    const ServerConfig cfg = makeConfig(TEST_POOL_SIZE);

    WorkQueue wq;
    ConnectionWorker worker(cfg, m_registry, m_activeConnections, m_logger);
    ThreadPool pool(cfg, wq, worker, m_sessionManager, m_activeConnections);

    pool.shutdown();
    pool.shutdown();
}

TEST_F(ThreadPoolTest, destructorJoinsThreads)
{
    const ServerConfig cfg = makeConfig(TEST_POOL_SIZE);
    WorkQueue wq;
    ConnectionWorker worker(cfg, m_registry, m_activeConnections, m_logger);

    {
        ThreadPool pool(cfg, wq, worker, m_sessionManager, m_activeConnections);
    }
}

TEST_F(ThreadPoolTest, noLeakAfterShutdownUnderLoad)
{
    const ServerConfig cfg = makeConfig(TEST_POOL_SIZE);

    WorkQueue wq;
    ConnectionWorker worker(cfg, m_registry, m_activeConnections, m_logger);
    ThreadPool pool(cfg, wq, worker, m_sessionManager, m_activeConnections);

    for (int i = 0; i < TEST_NUM_CONNECTIONS; ++i)
    {
        enqueueDummyConnection(wq);
    }

    pool.shutdown();
}

TEST_F(ThreadPoolTest, handlesHandshakeFailure)
{
    const ServerConfig cfg = makeConfig(TEST_POOL_SIZE);

    WorkQueue wq;
    ConnectionWorker worker(cfg, m_registry, m_activeConnections, m_logger);
    ThreadPool pool(cfg, wq, worker, m_sessionManager, m_activeConnections);

    const int fd = makeFd();
    ASSERT_GE(fd, 0);

    ::close(fd); // Simulate client disconnecting before handshake

    wq.enqueue({fd, LOCALHOST_IP});

    // Allow worker thread time to dequeue and hit the send error branch
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    pool.shutdown();
}
