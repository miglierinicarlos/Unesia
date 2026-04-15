#include "heartbeatMonitor.hpp"
#include "heartbeatHandler.hpp"
#include "logger.hpp"
#include "nodeRegistry.hpp"
#include "serverConfig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

namespace
{
    /// Builds a minimal ServerConfig with a custom heartbeat interval.
    ServerConfig makeConfig(uint32_t heartbeatIntervalSecs)
    {
        ServerConfig cfg {};
        cfg.m_port = ServerConfig::DEFAULT_PORT;
        cfg.m_idleTimeoutSecs = ServerConfig::DEFAULT_IDLE_TIMEOUT;
        cfg.m_threadPoolSize = ServerConfig::DEFAULT_THREAD_POOL_SIZE;
        cfg.m_maxClients = ServerConfig::DEFAULT_MAX_CLIENTS;
        cfg.m_heartbeatIntervalSecs = heartbeatIntervalSecs;
        return cfg;
    }

    /// Builds a NodeEntry with all fields populated.
    NodeRegistry::NodeEntry makeEntry(const std::string& nodeId,
                                      const std::string& bunker = "Bunker-A",
                                      const std::string& ip = "10.0.0.1",
                                      uint64_t cap = 100)
    {
        return {nodeId, bunker, ip, cap};
    }
} // namespace

// =============================================================================
// HeartbeatMonitorTest.monitorMarksOfflineAfterTwoMissedHeartbeats
// =============================================================================
TEST(HeartbeatMonitorTest, monitorMarksOfflineAfterTwoMissedHeartbeats)
{
    NodeRegistry reg;
    Logger logger(Logger::Level::NONE);
    const auto cfg = makeConfig(1); // 1 s interval → deadline at 2 s

    reg.registerNode(makeEntry("vault-01"));

    // Set last_seen_at to 3 s ago — beyond 2 × interval
    EXPECT_TRUE(reg.updateLastSeen("vault-01"));
    std::this_thread::sleep_for(std::chrono::seconds(3));

    HeartbeatMonitor monitor(reg, logger, cfg);
    monitor.start();

    // Give the monitor one full tick to detect the stale node
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    monitor.stop();

    EXPECT_TRUE(reg.isOffline("vault-01"));
}

// =============================================================================
// HeartbeatMonitorTest.monitorToleratesOneMissedHeartbeat
// =============================================================================
TEST(HeartbeatMonitorTest, monitorToleratesOneMissedHeartbeat)
{
    NodeRegistry reg;
    Logger logger(Logger::Level::NONE);
    const auto cfg = makeConfig(1); // deadline at 2 s

    reg.registerNode(makeEntry("vault-02"));

    // Set last_seen_at to ~1.3 s ago — past one interval but within two
    EXPECT_TRUE(reg.updateLastSeen("vault-02"));
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));

    HeartbeatMonitor monitor(reg, logger, cfg);
    monitor.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    monitor.stop();

    // Node must still be ONLINE — only one heartbeat missed
    EXPECT_FALSE(reg.isOffline("vault-02"));
}

// =============================================================================
// HeartbeatMonitorTest.monitorStopsCleanlyOnStop
// =============================================================================
TEST(HeartbeatMonitorTest, monitorStopsCleanlyOnStop)
{
    NodeRegistry reg;
    Logger logger(Logger::Level::NONE);
    const auto cfg = makeConfig(300); // very long interval — stop() must not wait 300 s

    HeartbeatMonitor monitor(reg, logger, cfg);
    monitor.start();

    const auto before = std::chrono::steady_clock::now();
    monitor.stop();
    const auto elapsed = std::chrono::steady_clock::now() - before;

    // stop() must return well under one interval — we allow 500 ms
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 500);
}

// =============================================================================
// HeartbeatMonitorTest.monitorStopsCleanlyWithoutStart
// =============================================================================
TEST(HeartbeatMonitorTest, monitorStopsCleanlyWithoutStart)
{
    NodeRegistry reg;
    Logger logger(Logger::Level::NONE);
    const auto cfg = makeConfig(5);

    // stop() on a never-started monitor must not crash or block
    HeartbeatMonitor monitor(reg, logger, cfg);
    EXPECT_NO_FATAL_FAILURE(monitor.stop());
}

// =============================================================================
// HeartbeatMonitorTest.noDataRaceMonitorVsWorkers
// =============================================================================
TEST(HeartbeatMonitorTest, noDataRaceMonitorVsWorkers)
{
    static constexpr int WORKER_COUNT = 16;
    static constexpr int HEARTBEATS_PER_WORKER = 50;

    NodeRegistry reg;
    Logger logger(Logger::Level::NONE);
    const auto cfg = makeConfig(1);

    // Register nodes that workers will send heartbeats for
    for (int i = 0; i < WORKER_COUNT; ++i)
    {
        reg.registerNode(makeEntry("vault-" + std::to_string(i)));
    }

    HeartbeatMonitor monitor(reg, logger, cfg);
    monitor.start();

    // 16 concurrent HeartbeatHandler workers hammering the registry
    std::vector<std::thread> workers;
    workers.reserve(WORKER_COUNT);

    for (int i = 0; i < WORKER_COUNT; ++i)
    {
        workers.emplace_back(
            [&reg, &logger, i]()
            {
                HeartbeatHandler handler(reg, logger);
                const std::string nodeId = "vault-" + std::to_string(i);
                for (int h = 0; h < HEARTBEATS_PER_WORKER; ++h)
                {
                    handler.handle(nodeId, "sess-" + std::to_string(i));
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            });
    }

    for (auto& w : workers)
    {
        w.join();
    }

    monitor.stop();

    // All nodes that kept sending heartbeats must still be ONLINE
    for (int i = 0; i < WORKER_COUNT; ++i)
    {
        const auto entry = reg.queryNode("vault-" + std::to_string(i));
        ASSERT_TRUE(entry.has_value());
        EXPECT_EQ(entry->status, NodeRegistry::NodeStatus::ONLINE)
            << "vault-" << i << " was incorrectly marked OFFLINE";
    }
}
