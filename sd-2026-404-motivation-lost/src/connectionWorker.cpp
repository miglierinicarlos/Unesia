#include "connectionWorker.hpp"
#include "messageSerializer.hpp"
#include "otelScope.hpp"
#include "metrics.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>

namespace
{
    // How often (ms) the HPC engine connection is polled for disconnection.
    // Short enough that disconnect is detected promptly; analyzeHandler owns all data reads.
    constexpr int HPC_ENGINE_POLL_TIMEOUT_MS = 500;
} // namespace

// Construction

ConnectionWorker::ConnectionWorker(const ServerConfig& config,
                                   NodeRegistry& nodeRegistry,
                                   std::atomic<uint32_t>& activeConnections,
                                   Logger& logger)
    : m_config(config)
    , m_nodeRegistry(nodeRegistry)
    , m_activeConnections(activeConnections)
    , m_logger(logger)
    , m_registerHandler(nodeRegistry, logger)
    , m_queryHandler(nodeRegistry, logger)
    , m_heartbeatHandler(nodeRegistry, logger)
    , m_analyzeHandler(nodeRegistry, config, logger)
{
}

// Public interface

void ConnectionWorker::handleConnection(int clientFd, const ConnectionContext& context)
{
    TeardownGuard guard(clientFd, m_activeConnections);

    if (!guard.isValid())
    {
        return;
    }

    const auto connectTime = std::chrono::steady_clock::now();
    DisconnectReason reason = DisconnectReason::RECV_ERROR;

    // activeNodeId is used to mark the node OFFLINE on disconnect.
    // Seeded from context (pre-authenticated sessions); updated on successful REGISTER.
    std::string activeNodeId = context.m_nodeId;

    // Created lazily only when a real protocol frame is received.
    std::unique_ptr<OtelScope> connectionSpan;

    auto ensureConnectionSpan = [&]()
    {
        if (connectionSpan)
        {
            return;
        }

        connectionSpan = std::make_unique<OtelScope>(context.m_sessionId, context.m_clientIp);
        connectionSpan->setAttribute("operation.name", "CLIENT_CONNECTION");
        connectionSpan->setAttribute("session.id", context.m_sessionId);
        connectionSpan->setAttribute("result", "OK");

        const uint32_t activeNow = m_activeConnections.load(std::memory_order_acquire);
        m_logger.logConnected({connectionSpan->traceId(), connectionSpan->spanId()},
                              {context.m_clientIp, context.m_sessionId, activeNow});
    };

    auto emitDisconnection = [&](DisconnectReason finalReason)
    {
        // If no span was created, this was likely a healthcheck TCP probe (connect/close only).
        // Skip logs/spans to avoid polluting traces.
        if (!connectionSpan)
        {
            return;
        }

        const auto durationMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - connectTime)
                .count();

        OtelScope disconnectionSpan("client_disconnection");
        disconnectionSpan.setAttribute("operation.name", "CLIENT_DISCONNECTION");
        disconnectionSpan.setAttribute("session.id", context.m_sessionId);
        disconnectionSpan.setAttribute("node.id", activeNodeId);
        disconnectionSpan.setAttribute("duration_ms", durationMs);
        disconnectionSpan.setAttribute("disconnect.reason", disconnectReasonToString(finalReason));

        if (finalReason == DisconnectReason::CLEAN_DISCONNECT)
        {
            disconnectionSpan.setAttribute("result", "OK");
        }
        else
        {
            disconnectionSpan.setAttribute("result", "ERROR");
            disconnectionSpan.recordError(disconnectReasonToString(finalReason));
        }

        m_logger.logDisconnected(
            {disconnectionSpan.traceId(), disconnectionSpan.spanId()},
            {activeNodeId, context.m_sessionId, durationMs, disconnectReasonToString(finalReason)});
    };

    if (!applyReceiveTimeout(guard.get()))
    {
        reason = DisconnectReason::SOCKET_CONFIG_ERROR;

        if (connectionSpan)
        {
            connectionSpan->setAttribute("result", "ERROR");
            connectionSpan->recordError("SO_RCVTIMEO failed");
        }

        markOfflineIfKnown(activeNodeId);
        emitDisconnection(reason);
        return;
    }

    // Sender captures the socket fd for use by message handlers.
    auto senderFn = [&guard](const uint8_t* data, std::size_t size) -> bool
    {
        return MessageSerializer::sendAll(guard.get(), data, size);
    };

    while (true)
    {
        // HPC engine connection: analyzeHandler owns all data reads on this fd.
        // Use passive poll to detect disconnection without consuming any frames.
        if (!activeNodeId.empty() && activeNodeId == m_config.m_hpcNodeId)
        {
            struct pollfd pfd
            {
                guard.get(), POLLIN | POLLHUP | POLLERR, 0
            };
            const int pr = poll(&pfd, 1, HPC_ENGINE_POLL_TIMEOUT_MS);
            if (pr < 0)
            {
                if (errno == EINTR)
                    continue;
                reason = DisconnectReason::RECV_ERROR;
                break;
            }
            if (pr == 0)
                continue;
            if ((pfd.revents & POLLHUP) != 0 || (pfd.revents & POLLERR) != 0)
            {
                reason = DisconnectReason::ABRUPT_DISCONNECT;
                break;
            }
            if ((pfd.revents & POLLIN) != 0)
            {
                // Peek to distinguish EOF (peer closed) from pending data (for analyzeHandler).
                uint8_t probe;
                const ssize_t n = recv(guard.get(), &probe, 1, MSG_PEEK | MSG_DONTWAIT);
                if (n == 0)
                {
                    reason = DisconnectReason::CLEAN_DISCONNECT;
                    break;
                }
                if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                {
                    reason = DisconnectReason::RECV_ERROR;
                    break;
                }
                // n > 0: ANALYZE_RESULT or other data waiting — analyzeHandler will read it
            }
            continue;
        }

        const auto result = MessageSerializer::readFrame(guard.get());

        if (result.status == MessageSerializer::FrameReadStatus::PEER_CLOSED)
        {
            reason = DisconnectReason::CLEAN_DISCONNECT;
            break;
        }

        if (result.status == MessageSerializer::FrameReadStatus::TIMEOUT)
        {
            reason = DisconnectReason::IDLE_TIMEOUT;
            break;
        }

        if (result.status == MessageSerializer::FrameReadStatus::IO_ERROR)
        {
            // ECONNRESET means the peer was killed (SIGKILL) or the connection was
            // forcibly reset. Treat it as abrupt disconnect — not a generic error.
            reason = isAbruptDisconnect(-1, errno) ? DisconnectReason::ABRUPT_DISCONNECT : DisconnectReason::RECV_ERROR;
            break;
        }

        // A valid protocol frame was received; this is real traffic.
        const auto reqStart = std::chrono::steady_clock::now();
        std::string reqResult = "ok";
        ensureConnectionSpan();

        // Dispatch by message type.
        switch (result.messageType)
        {
            case MessageSerializer::MessageType::REGISTER:
            {
                // Extract node_id for disconnect tracking before delegating.
                try
                {
                    const auto j = nlohmann::json::parse(result.payload);
                    if (j.contains("node_id") && j["node_id"].is_string())
                    {
                        activeNodeId = j["node_id"].get<std::string>();
                    }
                }
                catch (const std::exception& e)
                {
                    if (connectionSpan)
                        connectionSpan->recordError(std::string("JSON parse failed during node_id extraction: ") +
                                                    e.what());
                }
                catch (...)
                {
                    if (connectionSpan)
                        connectionSpan->recordError("Unknown exception during node_id extraction");
                }
                m_registerHandler.handle(result.messageId, result.payload, context.m_sessionId, senderFn);
                // Store fd after registerNode() creates the entry so it is not overwritten.
                if (!activeNodeId.empty())
                    m_nodeRegistry.setNodeFd(activeNodeId, guard.get());
                break;
            }

            case MessageSerializer::MessageType::QUERY_NODE:
            case MessageSerializer::MessageType::LIST_NODES:
                m_queryHandler.handle(
                    result.messageType, result.messageId, result.payload, context.m_sessionId, senderFn);
                break;

            case MessageSerializer::MessageType::HEARTBEAT:
                m_heartbeatHandler.handle(activeNodeId, context.m_sessionId);
                break;

            case MessageSerializer::MessageType::ANALYZE_GRAPH:
                m_analyzeHandler.handle(result.messageId, result.payload, context.m_sessionId, senderFn);
                break;

            default:
            {
                reqResult = "error";
                const auto errPayload = MessageSerializer::buildErrorPayload(
                    {result.messageId, MessageSerializer::ProtocolErrorCode::ERR_MALFORMED, "Unknown message type"});
                const auto errFrame = MessageSerializer::buildFrame(
                    MessageSerializer::MessageType::ERROR_MSG, result.messageId, errPayload);
                senderFn(errFrame.data(), errFrame.size());
                break;
            }
        }

        const auto reqEnd = std::chrono::steady_clock::now();
        const double duration = std::chrono::duration<double>(reqEnd - reqStart).count();
        Metrics::instance().observeRequestDuration(duration);
        
        const char* msgType = "UNKNOWN";
        switch (result.messageType) {
            case MessageSerializer::MessageType::REGISTER: msgType = "REGISTER"; break;
            case MessageSerializer::MessageType::QUERY_NODE: msgType = "QUERY_NODE"; break;
            case MessageSerializer::MessageType::LIST_NODES: msgType = "LIST_NODES"; break;
            case MessageSerializer::MessageType::HEARTBEAT: msgType = "HEARTBEAT"; break;
            case MessageSerializer::MessageType::ANALYZE_GRAPH: msgType = "ANALYZE_GRAPH"; break;
            case MessageSerializer::MessageType::ANALYZE_RESULT: msgType = "ANALYZE_RESULT"; break;
            default: msgType = "UNKNOWN"; break;
        }
        Metrics::instance().incrementRequestCount(msgType, reqResult);
    }

    if (connectionSpan && reason != DisconnectReason::CLEAN_DISCONNECT)
    {
        connectionSpan->setAttribute("result", "ERROR");
        connectionSpan->recordError(disconnectReasonToString(reason));
    }

    m_nodeRegistry.setNodeFd(activeNodeId, -1);
    markOfflineIfKnown(activeNodeId);
    emitDisconnection(reason);
}

// Private

bool ConnectionWorker::applyReceiveTimeout(int fd) const
{
    timeval timeout {};
    timeout.tv_sec = static_cast<time_t>(m_config.m_idleTimeoutSecs);
    timeout.tv_usec = 0;

    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
    {
        std::cerr << "[ERROR] connectionWorker: setsockopt(SO_RCVTIMEO) failed: "
                  << std::system_category().message(errno) << "\n";
        return false;
    }
    return true;
}

void ConnectionWorker::markOfflineIfKnown(const std::string& nodeId)
{
    if (!nodeId.empty())
    {
        m_nodeRegistry.setOffline(nodeId);
    }
}

bool ConnectionWorker::isAbruptDisconnect(ssize_t n, int err)
{
    // recv() returning -1 with ECONNRESET means the peer sent a TCP RST,
    // which happens when a process is killed with SIGKILL.
    return n < 0 && err == ECONNRESET;
}

const char* ConnectionWorker::disconnectReasonToString(DisconnectReason reason)
{
    switch (reason)
    {
        case DisconnectReason::IDLE_TIMEOUT: return "idle_timeout";
        case DisconnectReason::CLEAN_DISCONNECT: return "clean_disconnect";
        case DisconnectReason::ABRUPT_DISCONNECT: return "abrupt_disconnect";
        case DisconnectReason::RECV_ERROR: return "recv_error";
        case DisconnectReason::SOCKET_CONFIG_ERROR: return "socket_config_error";
        default: return "unknown";
    }
}
