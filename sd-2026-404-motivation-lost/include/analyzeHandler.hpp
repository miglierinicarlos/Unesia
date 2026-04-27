#pragma once

#include "logger.hpp"
#include "messageSerializer.hpp"
#include "nodeRegistry.hpp"
#include "serverConfig.hpp"

#include <cstdint>
#include <functional>
#include <string>

/**
 * @file analyzeHandler.hpp
 * @brief Handles ANALYZE_GRAPH (0x07) messages per ADR-003 and ADR-008.
 *
 * @details Implements the server-side dispatch logic for HPC graph analysis
 *          requests. The server acts as a router between any EOP client and
 *          the HPC engine node: it looks up the engine's socket fd in
 *          NodeRegistry, forwards the ANALYZE_GRAPH frame verbatim to the
 *          engine, waits for an ANALYZE_RESULT (0x08) response within a
 *          configurable timeout, and returns the result to the originating
 *          client.
 *
 *          Clients never connect to the HPC engine directly (ADR-008 AC2,
 *          AC3). All I/O to the engine fd is serialized by a per-node mutex
 *          held via NodeRegistry::acquireEngineFd(), ensuring that concurrent
 *          client dispatches do not interleave frames (ADR-002).
 *
 *          Error paths:
 *          - Engine node absent or OFFLINE → ERROR_SERVICE_UNAVAIL (0x04).
 *          - Engine does not respond within EOP_HPC_TIMEOUT seconds → ERROR_TIMEOUT (0x05).
 *          - I/O failure on engine fd → ERROR_SERVICE_UNAVAIL (0x04).
 *
 *          Dependencies are injected via the constructor to enable unit
 *          testing without live sockets.
 */
class AnalyzeHandler
{
public:
    /// Function type for sending raw bytes back to the originating client.
    using SendFn = std::function<bool(const uint8_t*, std::size_t)>;

    /**
     * @brief Constructs an AnalyzeHandler.
     *
     * @param registry Reference to the shared node registry (thread-safe).
     * @param config   Reference to the server configuration (read-only after startup).
     * @param logger   Reference to the shared structured logger.
     */
    AnalyzeHandler(NodeRegistry& registry, const ServerConfig& config, Logger& logger);

    AnalyzeHandler(const AnalyzeHandler&) = delete;
    AnalyzeHandler& operator=(const AnalyzeHandler&) = delete;
    AnalyzeHandler(AnalyzeHandler&&) = delete;
    AnalyzeHandler& operator=(AnalyzeHandler&&) = delete;

    /**
     * @brief Processes an ANALYZE_GRAPH message.
     *
     * @details Looks up the HPC engine node (identified by
     *          ServerConfig::m_hpcNodeId) in NodeRegistry. If found and ONLINE,
     *          acquires exclusive fd access, sets SO_RCVTIMEO to
     *          ServerConfig::m_hpcTimeoutSecs, forwards the ANALYZE_GRAPH
     *          frame to the engine, and awaits an ANALYZE_RESULT response.
     *          On success the ANALYZE_RESULT is forwarded verbatim to the
     *          caller via @p sender. On any failure an ERROR frame is sent.
     *
     * @param messageId   The message_id from the wire envelope (used for
     *                    request-response correlation with the originating client).
     * @param jsonPayload The raw JSON payload string from the ANALYZE_GRAPH frame.
     * @param sessionId   The session ID of the originating client (for logging).
     * @param sender      Callable that sends bytes to the originating client socket.
     *
     * @pre jsonPayload is a valid UTF-8 string (may or may not be valid JSON).
     * @post On engine ONLINE with valid fd and response within timeout:
     *       ANALYZE_RESULT frame forwarded to client via sender.
     * @post On engine absent or OFFLINE: ERROR frame with ERR_SERVICE_UNAVAIL sent.
     * @post On engine timeout: ERROR frame with ERR_TIMEOUT sent.
     */
    void handle(uint32_t messageId, const std::string& jsonPayload, const std::string& sessionId, const SendFn& sender);

private:
    NodeRegistry& m_registry;
    const ServerConfig& m_config;
    Logger& m_logger;
};
