#pragma once

#include "logger.hpp"
#include "messageSerializer.hpp"
#include "nodeRegistry.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>

/**
 * @file queryHandler.hpp
 * @brief Handles QUERY_NODE (0x04) and LIST_NODES (0x05) messages per ADR-003.
 *
 * @details Both message types are pure read operations against NodeRegistry
 *          and share the same response-building logic. The distinction is
 *          handled internally by dispatching on the message_type field.
 *
 *          Both operations acquire only a shared read lock on NodeRegistry
 *          (via NodeRegistry::queryNode and NodeRegistry::listNodes), so
 *          concurrent queries never block each other per ADR-002.
 *
 *          Dependencies are injected via the constructor to enable
 *          unit testing without live sockets.
 */
class QueryHandler
{
public:
    /// Function type for sending raw bytes to a client.
    using SendFn = std::function<bool(const uint8_t*, std::size_t)>;

    /**
     * @brief Constructs a QueryHandler.
     *
     * @param registry Reference to the shared node registry.
     * @param logger   Reference to the shared structured logger.
     */
    QueryHandler(NodeRegistry& registry, Logger& logger);

    QueryHandler(const QueryHandler&) = delete;
    QueryHandler& operator=(const QueryHandler&) = delete;
    QueryHandler(QueryHandler&&) = delete;
    QueryHandler& operator=(QueryHandler&&) = delete;

    /**
     * @brief Processes a QUERY_NODE or LIST_NODES message.
     *
     * @details Dispatches to the appropriate private handler based on
     *          messageType. Sends an ACK or ERROR response via sender.
     *
     * @param messageType The message type from the wire envelope (0x04 or 0x05).
     * @param messageId   The message_id from the wire envelope.
     * @param jsonPayload The raw JSON payload string (empty for LIST_NODES).
     * @param sessionId   The session ID for structured logging.
     * @param sender      Callable that sends bytes to the client socket.
     *
     * @pre messageType is QUERY_NODE or LIST_NODES.
     * @post On QUERY_NODE hit:  ACK frame with full NodeEntry metadata sent.
     * @post On QUERY_NODE miss: ERROR frame with ERR_NOT_FOUND sent.
     * @post On LIST_NODES:      ACK frame with nodes array sent.
     */
    void handle(MessageSerializer::MessageType messageType,
                uint32_t messageId,
                const std::string& jsonPayload,
                const std::string& sessionId,
                const SendFn& sender);

private:
    void handleQueryNode(uint32_t messageId,
                         const std::string& jsonPayload,
                         const std::string& sessionId,
                         const SendFn& sender);

    void handleListNodes(uint32_t messageId, const std::string& sessionId, const SendFn& sender);

    /// Converts a NodeEntry to a nlohmann::json object.
    static nlohmann::json entryToJson(const NodeRegistry::NodeEntry& entry);

    /// Converts a NodeStatus enum to its wire string representation.
    static const char* statusToString(NodeRegistry::NodeStatus status);

    /// Formats a system_clock time_point as an ISO 8601 UTC string.
    static std::string toIso8601(std::chrono::system_clock::time_point tp);

    NodeRegistry& m_registry;
    Logger& m_logger;
};
