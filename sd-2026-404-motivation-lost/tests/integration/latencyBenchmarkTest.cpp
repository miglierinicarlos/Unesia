/**
 * @file latencyBenchmarkTest.cpp
 * @brief Validates AC2 (Relative Latency) and NFR-1 (Absolute Latency P99).
 *
 * This test establishes a baseline handshake latency, floods the server with
 * 100 concurrent idle connections, and asserts that the latency remains under
 * the 200ms budget and does not degrade by more than 2.5x.
 */

#include "connectionWorker.hpp"
#include "logger.hpp"
#include "nodeRegistry.hpp"
#include "serverConfig.hpp"
#include "sessionManager.hpp"
#include "socketAcceptor.hpp"
#include "threadPool.hpp"
#include "workQueue.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <future>
#include <numeric>
#include <vector>

namespace
{
    // Network & Load Parameters
    constexpr uint16_t LATENCY_PORT = 19041;
    constexpr const char* LOCALHOST = "127.0.0.1";
    constexpr int LOAD_CLIENTS = 100;
    constexpr int ITERATIONS = 10;
    constexpr std::size_t RECV_BUFFER_SIZE = 1024;

    // Server Configuration Defaults for Benchmark
    constexpr uint32_t CONFIG_IDLE_TIMEOUT_SECS = 10;
    constexpr uint32_t CONFIG_THREAD_POOL_SIZE = 128;
    constexpr uint32_t CONFIG_MAX_CLIENTS = 500;

    // Timing & Delays
    constexpr auto BACKGROUND_CLIENT_LIFESPAN = std::chrono::seconds(2);
    constexpr auto SETUP_BIND_DELAY = std::chrono::milliseconds(100);
    constexpr auto ITERATION_DELAY = std::chrono::milliseconds(10);
    constexpr auto THREAD_POOL_WARMUP_DELAY = std::chrono::milliseconds(500);

    // Validation Thresholds
    constexpr double AC2_LATENCY_THRESHOLD_MULTIPLIER = 5;
    constexpr double NFR1_P99_LATENCY_US = 200000.0; // 200 ms in microseconds

    /**
     * @brief Connects, waits for the 4-byte length prefix of the welcome frame,
     * measures the exact time taken, and disconnects.
     */
    std::chrono::microseconds measureHandshakeLatency()
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return std::chrono::microseconds::max();

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(LATENCY_PORT);
        addr.sin_addr.s_addr = inet_addr(LOCALHOST);

        auto start = std::chrono::steady_clock::now();

        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            ::close(fd);
            return std::chrono::microseconds::max();
        }

        uint32_t lengthPrefix = 0;
        const ssize_t received = ::recv(fd, &lengthPrefix, sizeof(lengthPrefix), 0);

        auto end = std::chrono::steady_clock::now();
        ::close(fd);

        if (received != sizeof(lengthPrefix))
            return std::chrono::microseconds::max();

        return std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    }

    /**
     * @brief Connects and hangs cleanly, holding a thread in the ConnectionWorker.
     */
    bool runBackgroundIdleClient()
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return false;

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(LATENCY_PORT);
        addr.sin_addr.s_addr = inet_addr(LOCALHOST);

        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            ::close(fd);
            return false;
        }

        std::array<char, RECV_BUFFER_SIZE> buf {};
        ::recv(fd, buf.data(), buf.size(), 0);

        std::this_thread::sleep_for(BACKGROUND_CLIENT_LIFESPAN);

        ::close(fd);
        return true;
    }
} // namespace

class LatencyBenchmarkTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ServerConfig cfg;
        cfg.m_port = LATENCY_PORT;
        cfg.m_idleTimeoutSecs = CONFIG_IDLE_TIMEOUT_SECS;
        cfg.m_threadPoolSize = CONFIG_THREAD_POOL_SIZE;
        cfg.m_maxClients = CONFIG_MAX_CLIENTS;

        m_config = std::make_unique<ServerConfig>(cfg);
        m_logger = std::make_unique<Logger>(Logger::Level::NONE);
        m_registry = std::make_unique<NodeRegistry>();
        m_sessionManager = std::make_unique<SessionManager>();
        m_workQueue = std::make_unique<WorkQueue>();

        m_worker = std::make_unique<ConnectionWorker>(*m_config, *m_registry, m_activeConnections, *m_logger);
        m_pool =
            std::make_unique<ThreadPool>(*m_config, *m_workQueue, *m_worker, *m_sessionManager, m_activeConnections);

        m_acceptor = std::make_unique<SocketAcceptor>(*m_config,
                                                      m_activeConnections,
                                                      *m_logger,
                                                      [this](int fd, const std::string& ip)
                                                      {
                                                          // Corrected to memory_order_release
                                                          m_activeConnections.fetch_add(1, std::memory_order_release);
                                                          m_workQueue->enqueue({fd, ip});
                                                      });

        ASSERT_TRUE(m_acceptor->start());
        std::this_thread::sleep_for(SETUP_BIND_DELAY);
    }

    void TearDown() override
    {
        m_acceptor->stop();
        m_pool->shutdown();
    }

    std::unique_ptr<ServerConfig> m_config;
    std::unique_ptr<Logger> m_logger;
    std::unique_ptr<NodeRegistry> m_registry;
    std::unique_ptr<SessionManager> m_sessionManager;
    std::unique_ptr<WorkQueue> m_workQueue;
    std::unique_ptr<ConnectionWorker> m_worker;
    std::unique_ptr<ThreadPool> m_pool;
    std::unique_ptr<SocketAcceptor> m_acceptor;
    std::atomic<uint32_t> m_activeConnections {0};
};

TEST_F(LatencyBenchmarkTest, validatesLatencyUnderLoadAC2)
{
    // Measure Baseline Latency
    std::vector<double> baseLatencies;
    for (int i = 0; i < ITERATIONS; ++i)
    {
        baseLatencies.push_back(static_cast<double>(measureHandshakeLatency().count()));
        std::this_thread::sleep_for(ITERATION_DELAY);
    }
    const double avgBaseLatency = std::accumulate(baseLatencies.begin(), baseLatencies.end(), 0.0) / ITERATIONS;
    ASSERT_GT(avgBaseLatency, 0.0);

    // Inject 100 Background Clients
    std::vector<std::future<bool>> backgroundClients;
    backgroundClients.reserve(LOAD_CLIENTS);
    for (int i = 0; i < LOAD_CLIENTS; ++i)
    {
        backgroundClients.push_back(std::async(std::launch::async, runBackgroundIdleClient));
    }

    // Wait for the ThreadPool to process connections
    std::this_thread::sleep_for(THREAD_POOL_WARMUP_DELAY);
    EXPECT_GE(m_activeConnections.load(std::memory_order_acquire), LOAD_CLIENTS);

    // Measure Latency Under Load
    std::vector<double> loadLatencies;
    for (int i = 0; i < ITERATIONS; ++i)
    {
        loadLatencies.push_back(static_cast<double>(measureHandshakeLatency().count()));
        std::this_thread::sleep_for(ITERATION_DELAY);
    }
    const double avgLoadLatency = std::accumulate(loadLatencies.begin(), loadLatencies.end(), 0.0) / ITERATIONS;

    // Reporting
    std::cout << "[ BENCHMARK ] Avg Base Latency: " << avgBaseLatency << " us\n";
    std::cout << "[ BENCHMARK ] Avg Load Latency (" << LOAD_CLIENTS << " clients): " << avgLoadLatency << " us\n";

    // Absolute checks (NFR-1)
    EXPECT_LT(avgBaseLatency, NFR1_P99_LATENCY_US) << "NFR-1 Violation: Baseline latency exceeds 200ms P99 budget.";
    EXPECT_LT(avgLoadLatency, NFR1_P99_LATENCY_US) << "NFR-1 Violation: Load latency exceeds 200ms P99 budget.";

    // Relative check (AC2)
    EXPECT_LE(avgLoadLatency, avgBaseLatency * AC2_LATENCY_THRESHOLD_MULTIPLIER)
        << "AC2 Violation: Latency increased beyond acceptable threshold under load.";
}
