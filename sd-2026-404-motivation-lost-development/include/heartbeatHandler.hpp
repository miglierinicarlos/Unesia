#pragma once

#include "logger.hpp"
#include "nodeRegistry.hpp"

#include <string>

/**
 * @file heartbeatHandler.hpp
 * @brief Handles HEARTBEAT (0x06) messages per ADR-003.
 *
 * @details Fire-and-forget: updates lastSeenAt for the node associated with
 *          the current session. No response is sent to the client.
 *          Unknown nodeIds are silently ignored and logged at DEBUG level.
 */
class HeartbeatHandler
{
public:
    /**
     * @brief Constructs a HeartbeatHandler.
     *
     * @param registry Reference to the shared node registry.
     * @param logger   Reference to the shared structured logger.
     */
    HeartbeatHandler(NodeRegistry& registry, Logger& logger);

    HeartbeatHandler(const HeartbeatHandler&) = delete;
    HeartbeatHandler& operator=(const HeartbeatHandler&) = delete;
    HeartbeatHandler(HeartbeatHandler&&) = delete;
    HeartbeatHandler& operator=(HeartbeatHandler&&) = delete;

    /**
     * @brief Processes a HEARTBEAT message.
     *
     * @details Updates lastSeenAt in the registry for the given nodeId.
     *          If nodeId is unknown, logs at DEBUG level and returns.
     *          No response frame is written — the handler has no sender param
     *          by design to enforce the no-response contract (ADR-003).
     *
     * @param nodeId    The node_id associated with the current session.
     * @param sessionId The session ID for structured logging.
     */
    void handle(const std::string& nodeId, const std::string& sessionId);

private:
    NodeRegistry& m_registry;
    Logger& m_logger;
};
