#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/**
 * @file messageSerializer.hpp
 * @brief ADR-003 frame builder — assembles length-prefixed binary frames
 *        with a 10-byte fixed envelope and a JSON payload.
 *
 * This class is a pure serialization utility. It knows how to build any
 * ADR-003 frame given a message type, message ID, and a payload string,
 * but it has no knowledge of connections or business logic.
 *
 * Frame layout (ADR-003):
 * @code
 *   [4B frame_length (BE)] [1B protocol_version] [1B message_type]
 *   [4B message_id (BE)]   [4B payload_length (BE)] [payload bytes]
 * @endcode
 *
 * frame_length = ENVELOPE_SIZE + payload_length
 */
class MessageSerializer
{
public:
    /// Protocol version for v0.1 (ADR-003).
    static constexpr uint8_t PROTOCOL_VERSION = 0x01;

    /// Fixed envelope size in bytes (ADR-003).
    static constexpr uint32_t ENVELOPE_SIZE = 10;

    /// Size of the frame-length prefix in bytes.
    static constexpr uint32_t FRAME_LENGTH_PREFIX_SIZE = 4;

    /// Message type identifiers per ADR-003.
    enum class MessageType : uint8_t
    {
        REGISTER = 0x01,
        ACK = 0x02,
        ERROR_MSG = 0x03,
        QUERY_NODE = 0x04,
        LIST_NODES = 0x05,
        HEARTBEAT = 0x06,
    };

    /// Protocol-level error codes per ADR-003.
    enum class ProtocolErrorCode : uint8_t
    {
        ERR_DUPLICATE = 0x01,
        ERR_NOT_FOUND = 0x02,
        ERR_MALFORMED = 0x03,
        ERR_SERVICE_UNAVAIL = 0x04,
        ERR_TIMEOUT = 0x05,
        ERR_CAPACITY = 0x06,
        ERR_INTERNAL = 0xFF,
    };

    struct WelcomePayloadData
    {
        std::string serverVersion;
        std::string timestamp;
        std::string sessionId;
    };

    /// Data for building an ACK response payload (e.g. REGISTER success).
    struct AckPayloadData
    {
        uint32_t refMessageId {0};
        std::string nodeId;
    };

    /// Data for building an ERROR response payload.
    struct ErrorPayloadData
    {
        uint32_t refMessageId {0};
        ProtocolErrorCode errorCode {ProtocolErrorCode::ERR_INTERNAL};
        std::string description;
    };

    /**
     * @brief Builds a complete ADR-003 wire frame.
     *
     * The returned buffer is ready to be written to a TCP socket:
     * 4-byte length prefix + 10-byte envelope + payload bytes.
     *
     * @param type      Message type (ADR-003 enum).
     * @param messageId Per-connection monotonic ID for request-response correlation.
     * @param payload   UTF-8 JSON payload (may be empty for HEARTBEAT, LIST_NODES).
     * @return          Serialized frame as a byte vector.
     */
    [[nodiscard]] static std::vector<uint8_t>
    buildFrame(MessageType type, uint32_t messageId, const std::string& payload);

    /**
     * @brief Builds the JSON payload for the welcome message (ACK variant).
     *
     * Contains: server_version, timestamp (ISO-8601), session_id.
     *
     * @param serverVersion Version string (e.g. "0.1.0").
     * @param timestamp     ISO-8601 timestamp string.
     * @param sessionId     Hex-encoded session ID from SessionManager.
     * @return              Serialized JSON string.
     */
    [[nodiscard]] static std::string buildWelcomePayload(const WelcomePayloadData& data);

    /**
     * @brief Builds the JSON payload for an ACK response.
     *
     * Contains: ref_message_id, node_id.
     *
     * @param data ACK payload data.
     * @return     Serialized JSON string.
     */
    [[nodiscard]] static std::string buildAckPayload(const AckPayloadData& data);

    /**
     * @brief Builds the JSON payload for an ERROR response.
     *
     * Contains: ref_message_id, error_code, description.
     *
     * @param data Error payload data.
     * @return     Serialized JSON string.
     */
    [[nodiscard]] static std::string buildErrorPayload(const ErrorPayloadData& data);

    /// Outcome of a readFrame() call.
    enum class FrameReadStatus
    {
        OK,          ///< Frame read and parsed successfully.
        PEER_CLOSED, ///< Remote peer closed the connection cleanly.
        TIMEOUT,     ///< SO_RCVTIMEO expired or socket is non-blocking and no data available.
        IO_ERROR,    ///< Unrecoverable I/O error or malformed frame.
    };

    /// Result returned by readFrame().
    struct FrameReadResult
    {
        FrameReadStatus status {FrameReadStatus::IO_ERROR};
        MessageType messageType {MessageType::REGISTER};
        uint32_t messageId {0};
        std::string payload;
    };

    /**
     * @brief Reads one complete ADR-003 frame from a socket fd.
     *
     * Reads the 4-byte length prefix first, then the envelope and payload.
     * Handles EINTR internally. Returns the parsed fields on success.
     *
     * @param fd Connected socket file descriptor with SO_RCVTIMEO already set.
     * @return   FrameReadResult with status OK on success, or a failure status.
     */
    [[nodiscard]] static FrameReadResult readFrame(int fd);

    /**
     * @brief Sends all bytes in a buffer over a socket fd.
     *
     * Loops on send() to handle partial writes — TCP does not guarantee
     * that a single send() delivers all bytes. Returns false on any
     * unrecoverable error.
     *
     * @param fd   Connected socket file descriptor.
     * @param data Pointer to the buffer.
     * @param size Number of bytes to send.
     * @return     true if all bytes were delivered, false on error.
     */
    [[nodiscard]] static bool sendAll(int fd, const uint8_t* data, std::size_t size);
};
