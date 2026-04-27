#pragma once

#include "connectionWorker.hpp"
#include "serverConfig.hpp"
#include "sessionManager.hpp"
#include "workQueue.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

/**
 * @file threadPool.hpp
 * @brief Fixed-size thread pool whose workers consume accepted sockets from a
 * WorkQueue, perform the initial handshake, and dispatch them to ConnectionWorker.
 *
 * @details
 * Thread lifecycle strictly follows RAII. Threads are spawned during construction
 * and joined during destruction. Explicit shutdown is supported but not required.
 *
 * The thread pool is responsible for sending the initial Welcome message (ACK)
 * immediately upon dequeuing a connection, fulfilling the contract defined in
 * US-101 Task 3.
 */
class ThreadPool
{
public:
    ThreadPool(const ServerConfig& config,
               WorkQueue& workQueue,
               ConnectionWorker& connectionWorker,
               SessionManager& sessionManager,
               std::atomic<uint32_t>& activeConnections);

    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    void shutdown();
    [[nodiscard]] uint32_t workerCount() const;

private:
    void workerLoop();

    const ServerConfig& m_config;
    WorkQueue& m_workQueue;
    ConnectionWorker& m_connectionWorker;
    SessionManager& m_sessionManager;

    std::vector<std::thread> m_workers;
    std::atomic<uint32_t>& m_activeConnections;
    bool m_shutdown {false};
};
