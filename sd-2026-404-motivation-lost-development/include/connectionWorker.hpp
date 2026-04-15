#pragma once

#include "heartbeatHandler.hpp"
#include "logger.hpp"
#include "nodeRegistry.hpp"
#include "queryHandler.hpp"
#include "registerHandler.hpp"
#include "serverConfig.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <unistd.h>

/**
 * @file connectionWorker.hpp
 * @brief Per-connection worker that enforces idle timeout and teardown rules.
 *
 * On each connection:
 * - opens an OTel span and logs clientConnected (ADR-005)
 * - applies SO_RCVTIMEO using ServerConfig::m_idleTimeoutSecs
 * - loops on recv()
 * - handles clean disconnect / abrupt disconnect (ECONNRESET) / timeout / hard error
 * - logs clientDisconnected with duration and reason (ADR-005)
 * - marks node OFFLINE in NodeRegistry
 * - decrements active connection count atomically
 * - closes fd via RAII on all paths
 */
class ConnectionWorker
{
public:
    struct ConnectionContext
    {
        std::string m_nodeId {};
        std::string m_sessionId {};
        std::string m_clientIp {};
    };

    ConnectionWorker(const ServerConfig& config,
                     NodeRegistry& nodeRegistry,
                     std::atomic<uint32_t>& activeConnections,
                     Logger& logger);

    ConnectionWorker(const ConnectionWorker&) = delete;
    ConnectionWorker& operator=(const ConnectionWorker&) = delete;
    ConnectionWorker(ConnectionWorker&&) = delete;
    ConnectionWorker& operator=(ConnectionWorker&&) = delete;

    /**
     * @brief Handles a single accepted client fd until teardown.
     *
     * Ownership of clientFd is transferred to this method. The fd is
     * closed via RAII on every exit path.
     */
    void handleConnection(int clientFd, const ConnectionContext& context);

private:
    struct TeardownGuard
    {
        TeardownGuard(int fd, std::atomic<uint32_t>& activeConnections)
            : m_fd(fd)
            , m_activeConnections(activeConnections)
        {
        }

        ~TeardownGuard()
        {
            if (m_fd >= 0)
            {
                ::close(m_fd);
            }

            // Refactored to fetch_sub with memory_order_release (Addressed PR review comment).
            // Ensures the decrement is immediately visible to the SocketAcceptor's acquire load.
            m_activeConnections.fetch_sub(1, std::memory_order_release);
        }

        TeardownGuard(const TeardownGuard&) = delete;
        TeardownGuard& operator=(const TeardownGuard&) = delete;

        [[nodiscard]] int get() const
        {
            return m_fd;
        }
        [[nodiscard]] bool isValid() const
        {
            return m_fd >= 0;
        }

    private:
        int m_fd;
        std::atomic<uint32_t>& m_activeConnections;
    };

    enum class DisconnectReason
    {
        IDLE_TIMEOUT,
        CLEAN_DISCONNECT,
        ABRUPT_DISCONNECT, ///< recv()=0 with ECONNRESET — peer killed with SIGKILL.
        RECV_ERROR,
        SOCKET_CONFIG_ERROR
    };

    bool applyReceiveTimeout(int fd) const;
    void markOfflineIfKnown(const std::string& nodeId);

    static bool isAbruptDisconnect(ssize_t n, int err);
    static const char* disconnectReasonToString(DisconnectReason reason);

    const ServerConfig& m_config;
    NodeRegistry& m_nodeRegistry;
    std::atomic<uint32_t>& m_activeConnections;
    Logger& m_logger;
    RegisterHandler m_registerHandler;
    QueryHandler m_queryHandler;
    HeartbeatHandler m_heartbeatHandler;
};
