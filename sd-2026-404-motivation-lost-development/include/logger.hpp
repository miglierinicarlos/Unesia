#pragma once

#include <cstdint>
#include <mutex>
#include <string>

/**
 * @file logger.hpp
 * @brief Thread-safe structured JSON logger that writes to stdout per ADR-005.
 *
 * Every log line is a JSON object with the mandatory fields defined in
 * ADR-005: timestamp, level, service, traceId, spanId, message, and
 * any event-specific fields supplied by the caller.
 *
 * A single std::mutex protects the stdout write so that 16 concurrent
 * worker threads never produce interleaved output.
 *
 * No sensitive data (credentials, raw payload bytes) must ever appear
 * in a log line.
 */
class Logger
{
public:
    /// Log severity levels per ADR-005.
    enum class Level
    {
        INFO,
        WARN,
        ERROR,
        NONE
    };

    /**
     * @brief Fields emitted on every log line (ADR-005 mandatory).
     */
    struct BaseFields
    {
        std::string m_traceId; ///< OTel trace ID (hex string, 32 chars). Empty if no span.
        std::string m_spanId;  ///< OTel span ID (hex string, 16 chars). Empty if no span.
    };

    /**
     * @brief Fields emitted on clientConnected events (US-101 AC7).
     */
    struct ConnectedFields
    {
        std::string m_clientIp;
        std::string m_sessionId;
        uint32_t m_activeConnections {0};
    };

    /**
     * @brief Fields emitted on clientDisconnected events.
     */
    struct DisconnectedFields
    {
        std::string m_nodeId;
        std::string m_sessionId;
        int64_t m_connectionDurationMs {0};
        std::string m_reason;
    };

    /**
     * @brief Fields emitted when a REGISTER operation fails (duplicate or malformed).
     */
    struct NodeRegisterErrorFields
    {
        std::string m_nodeId; ///< May be empty if payload was malformed.
        std::string m_sessionId;
        std::string m_reason;
    };

    /**
     * @brief Fields emitted on QUERY_NODE operations (hit and miss).
     */
    struct NodeQueriedFields
    {
        std::string m_nodeId;
        std::string m_sessionId;
        std::string m_result; ///< "OK" or "ERR_NOT_FOUND".
    };

    /**
     * @brief Fields emitted on LIST_NODES operations.
     */
    struct NodesListedFields
    {
        std::string m_sessionId;
        std::size_t m_nodeCount {0};
    };

    /**
     * @brief Fields emitted on node_registered events (US-103 AC2).
     */
    struct NodeRegisteredFields
    {
        std::string m_nodeId;
        std::string m_sessionId;
    };

    /**
     * @brief Fields emitted on heartbeat_received events (US-104).
     */
    struct HeartbeatReceivedFields
    {
        std::string m_nodeId;
        std::string m_sessionId;
    };

    /**
     * @brief Fields emitted when a heartbeat arrives for an unknown node (US-104).
     */
    struct HeartbeatUnknownNodeFields
    {
        std::string m_nodeId;
        std::string m_sessionId;
    };

    /**
     * @brief Fields emitted when a node is marked OFFLINE by the heartbeat monitor.
     */
    struct NodeOfflineFields
    {
        std::string m_nodeId;
    };

    explicit Logger(Level minLevel = Level::INFO);

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    /**
     * @brief Logs a clientConnected event to stdout as structured JSON.
     *
     * @param base    Trace and span IDs for the current OTel span.
     * @param fields  Connection-specific fields.
     */
    void logConnected(const BaseFields& base, const ConnectedFields& fields);

    /**
     * @brief Logs a clientDisconnected event to stdout as structured JSON.
     *
     * @param base    Trace and span IDs for the current OTel span.
     * @param fields  Disconnection-specific fields.
     */
    void logDisconnected(const BaseFields& base, const DisconnectedFields& fields);

    /**
     * @brief Logs a node_registered event to stdout as structured JSON.
     *
     * @param base    Trace and span IDs for the current OTel span.
     * @param fields  Node registration-specific fields.
     */
    void logNodeRegistered(const BaseFields& base, const NodeRegisteredFields& fields);

    /**
     * @brief Logs a failed REGISTER operation (duplicate or malformed payload).
     *
     * @param base    Trace and span IDs for the current OTel span.
     * @param fields  Error-specific fields.
     */
    void logNodeRegisterError(const BaseFields& base, const NodeRegisterErrorFields& fields);

    /**
     * @brief Logs a QUERY_NODE operation (hit or miss).
     *
     * @param base    Trace and span IDs for the current OTel span.
     * @param fields  Query-specific fields including result.
     */
    void logNodeQueried(const BaseFields& base, const NodeQueriedFields& fields);

    /**
     * @brief Logs a LIST_NODES operation.
     *
     * @param base    Trace and span IDs for the current OTel span.
     * @param fields  List-specific fields.
     */
    void logNodesListed(const BaseFields& base, const NodesListedFields& fields);

    /**
     * @brief Logs a plain message at the given level.
     *
     * Used for general server events (startup, shutdown, errors) where
     * no OTel span context is available.
     *
     * @param level   Severity level.
     * @param message Human-readable message. Must not contain sensitive data.
     */
    void log(Level level, const std::string& message);

    /**
     * @brief Logs a heartbeat_received event (node known and ONLINE).
     */
    void logHeartbeatReceived(const BaseFields& base, const HeartbeatReceivedFields& fields);

    /**
     * @brief Logs a heartbeat for an unknown node_id at WARN level.
     */
    void logHeartbeatUnknownNode(const BaseFields& base, const HeartbeatUnknownNodeFields& fields);

    /**
     * @brief Logs a node_offline transition at WARN level (US-104 AC4).
     *
     * @param base   Trace and span IDs for the current OTel span.
     * @param fields Node-specific fields.
     */
    void logNodeOffline(const BaseFields& base, const NodeOfflineFields& fields);

private:
    /// Writes a raw JSON string to stdout under the mutex.
    void writeToStdout(const std::string& json);

    /// Returns the current UTC time as an ISO-8601 string.
    [[nodiscard]] static std::string nowIso8601Utc();

    /// Converts a Level enum to its string representation.
    [[nodiscard]] static const char* levelToString(Level level);

    Level m_minLevel;
    std::mutex m_mutex;
};
