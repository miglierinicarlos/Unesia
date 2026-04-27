#pragma once

#include "logger.hpp"
#include "serverConfig.hpp"

#include <atomic>
#include <functional>
#include <string>
#include <thread>

/**
 * @file socketAcceptor.hpp
 * @brief TCP listening socket that accepts incoming connections and dispatches
 * them via a caller-supplied callback.
 *
 * Enforces EOP_MAX_CLIENTS (ADR-002, US-102 Task 9). If the server is at
 * capacity, it rejects the connection inline with an ERR_CAPACITY frame
 * and closes the socket to prevent work queue saturation.
 */
class SocketAcceptor
{
public:
    /**
     * @brief Callback invoked once per accepted connection.
     *
     * @param fd Accepted socket file descriptor.
     * @param ip Client IP address extracted from the sockaddr.
     */
    using OnAcceptCallback = std::function<void(int fd, const std::string& ip)>;

    /**
     * @brief Constructs a SocketAcceptor.
     *
     * @param config            Server configuration.
     * @param activeConnections Shared atomic counter of currently active clients.
     * @param logger            Shared logger for reporting capacity rejections.
     * @param onAccept          Called with each accepted fd and IP.
     */
    explicit SocketAcceptor(const ServerConfig& config,
                            std::atomic<uint32_t>& activeConnections,
                            Logger& logger,
                            OnAcceptCallback onAccept);

    ~SocketAcceptor();

    SocketAcceptor(const SocketAcceptor&) = delete;
    SocketAcceptor& operator=(const SocketAcceptor&) = delete;
    SocketAcceptor(SocketAcceptor&&) = delete;
    SocketAcceptor& operator=(SocketAcceptor&&) = delete;

    bool start();
    void stop();

private:
    struct FdGuard
    {
        FdGuard() = default;
        explicit FdGuard(int fd)
            : m_fd(fd)
        {
        }
        ~FdGuard()
        {
            reset();
        }

        FdGuard(const FdGuard&) = delete;
        FdGuard& operator=(const FdGuard&) = delete;

        FdGuard(FdGuard&& other) noexcept
            : m_fd(other.m_fd)
        {
            other.m_fd = -1;
        }
        FdGuard& operator=(FdGuard&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_fd = other.m_fd;
                other.m_fd = -1;
            }
            return *this;
        }

        void reset()
        {
            if (m_fd >= 0)
            {
                ::close(m_fd);
                m_fd = -1;
            }
        }

        [[nodiscard]] int get() const
        {
            return m_fd;
        }
        [[nodiscard]] bool isValid() const
        {
            return m_fd >= 0;
        }

    private:
        int m_fd {-1};
    };

    void acceptLoop();

    const ServerConfig& m_config;
    std::atomic<uint32_t>& m_activeConnections;
    Logger& m_logger;
    OnAcceptCallback m_onAccept;
    FdGuard m_listenFd;
    std::thread m_acceptThread;
    std::atomic<bool> m_stopFlag {false};
};
