#pragma once

#include "logger.hpp"
#include "messageSerializer.hpp"
#include "nodeRegistry.hpp"

#include <cstdint>
#include <functional>
#include <string>

/**
 * @file registerHandler.hpp
 * @brief Handles REGISTER (0x01) messages per ADR-003.
 *
 * @details Parses the JSON payload from a REGISTER frame, validates
 *          required fields (node_id, bunker_name, ip_address, capacity),
 *          attempts registration via NodeRegistry, and sends either an
 *          ACK or ERROR response frame.
 *
 *          Follows a strict validate -> act -> respond sequence with no
 *          shared state of its own. Dependencies are injected via the
 *          constructor to enable unit testing without live sockets.
 */
class RegisterHandler
{
public:
    /// Function type for sending raw bytes to a client.
    using SendFn = std::function<bool(const uint8_t*, std::size_t)>;

    /**
     * @brief Constructs a RegisterHandler.
     *
     * @param registry Reference to the shared node registry.
     * @param logger   Reference to the shared structured logger.
     */
    RegisterHandler(NodeRegistry& registry, Logger& logger);

    RegisterHandler(const RegisterHandler&) = delete;
    RegisterHandler& operator=(const RegisterHandler&) = delete;
    RegisterHandler(RegisterHandler&&) = delete;
    RegisterHandler& operator=(RegisterHandler&&) = delete;

    /**
     * @brief Processes a REGISTER message.
     *
     * @details Validates the JSON payload, registers the node, and sends
     *          an ACK or ERROR response via the provided sender function.
     *
     * @param messageId   The message_id from the wire envelope.
     * @param jsonPayload The raw JSON payload string.
     * @param sessionId   The session ID for structured logging.
     * @param sender      Callable that sends bytes to the client socket.
     *
     * @pre jsonPayload is a valid UTF-8 string (may or may not be valid JSON).
     * @post On success: ACK frame sent via sender, node registered in registry.
     * @post On duplicate: ERROR frame sent via sender with ERR_DUPLICATE.
     * @post On malformed: ERROR frame sent via sender with ERR_MALFORMED.
     */
    void handle(uint32_t messageId, const std::string& jsonPayload, const std::string& sessionId, const SendFn& sender);

private:
    NodeRegistry& m_registry;
    Logger& m_logger;
};
