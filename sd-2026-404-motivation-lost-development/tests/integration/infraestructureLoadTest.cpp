/**
 * @file infraestructureLoadTest.cpp
 * @brief Infrastructure Load and Resource tests
 *
 * Validates the raw capacity and resource safety of the SocketAcceptor,
 * ThreadPool, and ConnectionWorker by bombarding the server with raw
 * byte streams. Asserts zero file descriptor leaks and safe teardowns.
 */

#include "connectionWorker.hpp"
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
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string_view>
#include <vector>

namespace
{
    // Test Parameters
    constexpr uint16_t LOAD_PORT = 19040;
    constexpr const char* LOCALHOST = "127.0.0.1";

    constexpr uint32_t LOAD_IDLE_TIMEOUT_SECS = 10;
    constexpr uint32_t LOAD_THREAD_POOL_SIZE = 32;
    constexpr uint32_t LOAD_MAX_CLIENTS = 2000;

    // FD test specific load
    constexpr int FD_TEST_BATCH_SIZE = 200;
    constexpr int FD_TEST_TARGET_CYCLES_LOCAL = 100000;
    constexpr int FD_TEST_TARGET_CYCLES_CI = 5000;

    // Pre-calculate a valid ADR-003 HEARTBEAT frame to prevent server-side parsing errors (AC8).
    // This allows testing L4 throughput without triggering L7 protocol violations.
    const std::vector<uint8_t> VALID_HEARTBEAT_FRAME = []() -> std::vector<uint8_t>
    {
        return MessageSerializer::buildFrame(MessageSerializer::MessageType::HEARTBEAT, 0, "");
    }();

    // Thread-safe single evaluation of CI environment variables
    const bool IS_RUNNING_IN_CI = []() -> bool
    {
        const char* ga = std::getenv("GITHUB_ACTIONS"); // NOLINT(concurrency-mt-unsafe)
        const char* ci = std::getenv("CI");             // NOLINT(concurrency-mt-unsafe)
        return (ga != nullptr) || (ci != nullptr);
    }();

    // Workload dimensions scale down in CI to prevent runner timeouts
    const int LOAD_NUM_CLIENTS = IS_RUNNING_IN_CI ? 100 : 1000;
    const int LOAD_CHUNKS_PER_CLIENT = IS_RUNNING_IN_CI ? 1000 : 10000;

    // Helpers
    /**
     * @brief Raises the open file descriptor limit for the test process.
     */
    void setupLimits()
    {
        struct rlimit rl;
        if (::getrlimit(RLIMIT_NOFILE, &rl) == 0)
        {
            rl.rlim_cur = 4096;
            rl.rlim_max = 4096;
            ::setrlimit(RLIMIT_NOFILE, &rl);
        }
    }

    /**
     * @brief Safely counts open file descriptors for the current process.
     * Uses C++17 std::filesystem, which avoids the concurrency-mt-unsafe
     * warnings associated with POSIX readdir().
     */
    int countOpenFds()
    {
        std::error_code ec;
        auto dirIter = std::filesystem::directory_iterator("/proc/self/fd", ec);
        if (ec)
        {
            return -1;
        }

        int count = 0;
        for (const auto& entry : dirIter)
        {
            (void)entry; // Suppress unused variable warning
            ++count;
        }
        return count;
    }

    /**
     * @brief Configuration for the dumb client workload.
     * Encapsulated in a struct to prevent bugprone-easily-swappable-parameters.
     */
    struct DumbClientConfig
    {
        uint16_t port;
        int chunksToSend;
    };

    /**
     * @brief Minimal client: Connects, optionally streams raw bytes, and disconnects.
     */
    bool runDumbClient(DumbClientConfig config)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return false;

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config.port);
        addr.sin_addr.s_addr = inet_addr(LOCALHOST);

        constexpr int CONNECT_RETRIES = 5;
        constexpr auto CONNECT_RETRY_DELAY = std::chrono::milliseconds(10);

        bool connected = false;
        for (int attempt = 0; attempt < CONNECT_RETRIES; ++attempt)
        {
            if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0)
            {
                connected = true;
                break;
            }

            std::this_thread::sleep_for(CONNECT_RETRY_DELAY);
        }

        if (!connected)
        {
            ::close(fd);
            return false;
        }

        for (int i = 0; i < config.chunksToSend; ++i)
        {
            const ssize_t sent = ::send(fd, VALID_HEARTBEAT_FRAME.data(), VALID_HEARTBEAT_FRAME.size(), MSG_NOSIGNAL);
            if (sent < 0)
            {
                ::close(fd);
                return false; // Server collapsed, dropped connection, or reset peer
            }
        }

        ::close(fd);
        return true;
    }
} // namespace

// Fixture
class InfrastructureLoadTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        setupLimits();

        ServerConfig cfg;
        cfg.m_port = LOAD_PORT;
        cfg.m_idleTimeoutSecs = LOAD_IDLE_TIMEOUT_SECS;
        cfg.m_threadPoolSize = LOAD_THREAD_POOL_SIZE;
        cfg.m_maxClients = LOAD_MAX_CLIENTS;

        s_config = std::make_unique<ServerConfig>(cfg);
        s_logger = std::make_unique<Logger>(Logger::Level::NONE);
        s_registry = std::make_unique<NodeRegistry>();
        s_sessionManager = std::make_unique<SessionManager>();
        s_workQueue = std::make_unique<WorkQueue>();

        s_worker = std::make_unique<ConnectionWorker>(*s_config, *s_registry, s_activeConnections, *s_logger);
        s_pool =
            std::make_unique<ThreadPool>(*s_config, *s_workQueue, *s_worker, *s_sessionManager, s_activeConnections);

        s_acceptor = std::make_unique<SocketAcceptor>(*s_config,
                                                      s_activeConnections,
                                                      *s_logger,
                                                      [](int fd, const std::string& ip)
                                                      {
                                                          s_activeConnections.fetch_add(1, std::memory_order_relaxed);
                                                          s_workQueue->enqueue({fd, ip});
                                                      });

        ASSERT_TRUE(s_acceptor->start());
        // Yield momentarily to ensure the listen socket is fully bound
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    static void TearDownTestSuite()
    {
        if (s_acceptor)
            s_acceptor->stop();
        if (s_pool)
            s_pool->shutdown();

        s_acceptor.reset();
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
    inline static std::unique_ptr<SocketAcceptor> s_acceptor;
    inline static std::atomic<uint32_t> s_activeConnections {0};
};

// Tests
TEST_F(InfrastructureLoadTest, verifyConcurrentThroughputAndNoErrors)
{
    std::vector<std::future<bool>> futures;
    futures.reserve(static_cast<std::size_t>(LOAD_NUM_CLIENTS));

    for (int i = 0; i < LOAD_NUM_CLIENTS; ++i)
    {
        futures.push_back(
            std::async(std::launch::async, runDumbClient, DumbClientConfig {LOAD_PORT, LOAD_CHUNKS_PER_CLIENT}));
    }

    int successCount = 0;
    for (auto& f : futures)
    {
        if (f.get())
        {
            successCount++;
        }
    }

    EXPECT_EQ(successCount, LOAD_NUM_CLIENTS)
        << "Some clients failed to complete their workload. The ThreadPool or SocketAcceptor may be deadlocked.";
}

TEST_F(InfrastructureLoadTest, verifyNoFdLeaksAfterLoad)
{
    // Wait until all workers from the previous test have fully torn down
    constexpr int MAX_DRAIN_WAIT_MS = 60000;
    constexpr int POLL_INTERVAL_MS = 50;
    int waited = 0;
    while (s_activeConnections.load(std::memory_order_acquire) > 0 && waited < MAX_DRAIN_WAIT_MS)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
        waited += POLL_INTERVAL_MS;
    }

    ASSERT_EQ(s_activeConnections.load(std::memory_order_acquire), 0)
        << "Previous test left active connections after " << MAX_DRAIN_WAIT_MS << "ms — cannot take clean baseline.";

    // One extra sleep to let the OS reclaim any fds closed by the last workers
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const int fdsBefore = countOpenFds();
    ASSERT_GT(fdsBefore, 0);

    const int targetCycles = IS_RUNNING_IN_CI ? FD_TEST_TARGET_CYCLES_CI : FD_TEST_TARGET_CYCLES_LOCAL;

    for (int cycle = 0; cycle < targetCycles; cycle += FD_TEST_BATCH_SIZE)
    {
        std::vector<std::future<bool>> futures;
        const int currentBatch = std::min(FD_TEST_BATCH_SIZE, targetCycles - cycle);
        futures.reserve(static_cast<std::size_t>(currentBatch));

        for (int i = 0; i < currentBatch; ++i)
        {
            futures.push_back(std::async(std::launch::async, runDumbClient, DumbClientConfig {LOAD_PORT, 0}));
        }

        for (auto& f : futures)
        {
            f.get();
        }
    }

    // Poll until workers drain — same strategy as baseline wait
    waited = 0;
    while (s_activeConnections.load(std::memory_order_acquire) > 0 && waited < MAX_DRAIN_WAIT_MS)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
        waited += POLL_INTERVAL_MS;
    }

    EXPECT_EQ(s_activeConnections.load(std::memory_order_acquire), 0)
        << "Phantom connections remain after all cycles completed.";

    const int fdsAfter = countOpenFds();

    EXPECT_NEAR(fdsBefore, fdsAfter, 2) << "File descriptor leak detected (Before: " << fdsBefore
                                        << ", After: " << fdsAfter << ").";
}
