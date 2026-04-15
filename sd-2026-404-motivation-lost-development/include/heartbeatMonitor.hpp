#pragma once

#include "logger.hpp"
#include "nodeRegistry.hpp"
#include "serverConfig.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>

/**
 * @file heartbeatMonitor.hpp
 * @brief Background thread that detects stale nodes and marks them OFFLINE.
 *
 * @details Wakes every EOP_HEARTBEAT_INTERVAL seconds and marks any ONLINE
 *          node whose last_seen_at is older than 2 × EOP_HEARTBEAT_INTERVAL
 *          as OFFLINE (ADR-003: one missed heartbeat tolerated).
 *
 *          Two-phase locking strategy: stale candidates are collected under a
 *          shared read lock first; the exclusive write lock is held only during
 *          the setOffline() calls, minimising contention with concurrent reads
 *          (QUERY_NODE, LIST_NODES) per ADR-002.
 *
 *          stop() interrupts the current sleep immediately via
 *          condition_variable::wait_for() — no busy-wait, no indefinite block.
 */
class HeartbeatMonitor
{
public:
    /**
     * @brief Constructs the monitor. Does not start the background thread.
     *
     * @param registry Reference to the shared node registry.
     * @param logger   Reference to the shared structured logger.
     * @param config   Server configuration (reads m_heartbeatIntervalSecs).
     */
    HeartbeatMonitor(NodeRegistry& registry, Logger& logger, const ServerConfig& config);

    ~HeartbeatMonitor();

    HeartbeatMonitor(const HeartbeatMonitor&) = delete;
    HeartbeatMonitor& operator=(const HeartbeatMonitor&) = delete;
    HeartbeatMonitor(HeartbeatMonitor&&) = delete;
    HeartbeatMonitor& operator=(HeartbeatMonitor&&) = delete;

    /**
     * @brief Starts the background monitoring thread.
     *
     * @pre start() has not been called before on this instance.
     */
    void start();

    /**
     * @brief Stops the background thread and blocks until it exits.
     *
     * @details Signals the condition variable so the sleeping thread wakes
     *          immediately. Safe to call if start() was never called.
     */
    void stop();

private:
    /// Entry point for the background thread.
    void run();

    /// Performs one scan: collects stale node IDs under shared lock,
    /// then transitions each to OFFLINE under exclusive lock.
    void tick();

    NodeRegistry& m_registry;
    Logger& m_logger;
    const uint32_t m_intervalSecs;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop {false};
    std::thread m_thread;
};
