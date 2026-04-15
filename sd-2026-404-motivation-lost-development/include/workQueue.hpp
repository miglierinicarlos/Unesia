#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <string>

/**
 * @file workQueue.hpp
 * @brief Thread-safe producer-consumer queue for accepted connections.
 *
 * @details
 * Implements the work distribution mechanism mandated by ADR-002. The
 * SocketAcceptor (producer) calls enqueue() after each accept(); the
 * ThreadPool worker threads (consumers) call dequeue(), which blocks until
 * a connection is available or stop() has been called.
 *
 * @invariant After stop() returns, all subsequent dequeue() calls return
 * std::nullopt immediately. Any fd still in the queue at that
 * point is closed to prevent descriptor leaks.
 */

struct AcceptedSocket
{
    int fd;
    std::string ip;
};

class WorkQueue
{
public:
    WorkQueue() = default;
    ~WorkQueue() = default;

    WorkQueue(const WorkQueue&) = delete;
    WorkQueue& operator=(const WorkQueue&) = delete;
    WorkQueue(WorkQueue&&) = delete;
    WorkQueue& operator=(WorkQueue&&) = delete;

    /**
     * @brief Enqueues an accepted socket connection.
     *
     * @param socket Struct containing the fd and client IP.
     */
    void enqueue(AcceptedSocket socket);

    /**
     * @brief Dequeues the next socket connection, blocking until available.
     *
     * @return The next connection wrapped in std::optional, or std::nullopt
     * if the queue is empty and stop() has been called.
     */
    [[nodiscard]] std::optional<AcceptedSocket> dequeue();

    /**
     * @brief Signals consumers to wake and drain the queue, closing pending fds.
     */
    void stop();

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool isStopped() const;

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::queue<AcceptedSocket> m_queue;
    bool m_stopped {false};
};
