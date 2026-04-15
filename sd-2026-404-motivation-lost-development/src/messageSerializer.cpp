#include "messageSerializer.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <system_error>

namespace
{
    // Maximum allowed frame payload to prevent unreasonable allocations.
    constexpr uint32_t MAX_FRAME_PAYLOAD = 65536;

    // Reads exactly n bytes from fd into buf, retrying on EINTR.
    // Returns PEER_CLOSED if the peer shut down, TIMEOUT on EAGAIN/EWOULDBLOCK,
    // IO_ERROR on any other failure.
    MessageSerializer::FrameReadStatus readExact(int fd, uint8_t* buf, std::size_t n)
    {
        std::size_t received = 0;

        while (received < n)
        {
            const ssize_t r = ::recv(fd, buf + received, n - received, 0);

            if (r > 0)
            {
                received += static_cast<std::size_t>(r);
                continue;
            }

            if (r == 0)
            {
                return MessageSerializer::FrameReadStatus::PEER_CLOSED;
            }

            if (errno == EINTR)
            {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return MessageSerializer::FrameReadStatus::TIMEOUT;
            }

            return MessageSerializer::FrameReadStatus::IO_ERROR;
        }

        return MessageSerializer::FrameReadStatus::OK;
    }
} // namespace

std::vector<uint8_t> MessageSerializer::buildFrame(MessageType type, uint32_t messageId, const std::string& payload)
{
    const uint32_t payloadLen = static_cast<uint32_t>(payload.size());
    const uint32_t frameLen = ENVELOPE_SIZE + payloadLen;
    const std::size_t totalSize = FRAME_LENGTH_PREFIX_SIZE + frameLen;

    std::vector<uint8_t> frame(totalSize);
    std::size_t offset = 0;

    auto writeU32BE = [&](uint32_t value)
    {
        frame[offset++] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
        frame[offset++] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
        frame[offset++] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
        frame[offset++] = static_cast<uint8_t>(value & 0xFFU);
    };

    // 4-byte frame length prefix (big-endian)
    writeU32BE(frameLen);

    // Envelope byte 0: protocol version
    frame[offset++] = PROTOCOL_VERSION;

    // Envelope byte 1: message type
    frame[offset++] = static_cast<uint8_t>(type);

    // Envelope bytes 2-5: message_id (big-endian)
    writeU32BE(messageId);

    // Envelope bytes 6-9: payload_length (big-endian)
    writeU32BE(payloadLen);

    // Payload
    if (payloadLen > 0)
    {
        std::memcpy(frame.data() + offset, payload.data(), payloadLen);
    }

    return frame;
}

std::string MessageSerializer::buildWelcomePayload(const WelcomePayloadData& data)
{
    nlohmann::json j;
    j["server_version"] = data.serverVersion;
    j["timestamp"] = data.timestamp;
    j["session_id"] = data.sessionId;
    return j.dump();
}

std::string MessageSerializer::buildAckPayload(const AckPayloadData& data)
{
    nlohmann::json j;
    j["ref_message_id"] = data.refMessageId;
    j["node_id"] = data.nodeId;
    return j.dump();
}

std::string MessageSerializer::buildErrorPayload(const ErrorPayloadData& data)
{
    nlohmann::json j;
    j["ref_message_id"] = data.refMessageId;
    j["error_code"] = static_cast<uint8_t>(data.errorCode);
    j["description"] = data.description;
    return j.dump();
}

MessageSerializer::FrameReadResult MessageSerializer::readFrame(int fd)
{
    // Step 1: read the 4-byte frame length prefix.
    uint8_t lenBuf[FRAME_LENGTH_PREFIX_SIZE];
    const auto lenStatus = readExact(fd, lenBuf, FRAME_LENGTH_PREFIX_SIZE);
    if (lenStatus != FrameReadStatus::OK)
    {
        return {lenStatus, {}, 0, {}};
    }

    uint32_t frameLen = 0;
    std::memcpy(&frameLen, lenBuf, sizeof(frameLen));
    frameLen = ntohl(frameLen);

    // Reject frames that are too small to contain a valid envelope.
    if (frameLen < ENVELOPE_SIZE)
    {
        return {FrameReadStatus::IO_ERROR, {}, 0, {}};
    }

    const uint32_t payloadLen = frameLen - ENVELOPE_SIZE;
    if (payloadLen > MAX_FRAME_PAYLOAD)
    {
        return {FrameReadStatus::IO_ERROR, {}, 0, {}};
    }

    // Step 2: read the envelope (10 bytes).
    uint8_t envelope[ENVELOPE_SIZE];
    const auto envStatus = readExact(fd, envelope, ENVELOPE_SIZE);
    if (envStatus != FrameReadStatus::OK)
    {
        return {envStatus, {}, 0, {}};
    }

    // envelope[0] = protocol_version (not validated here — callers may check)
    const auto msgType = static_cast<MessageType>(envelope[1]);

    uint32_t msgIdBE = 0;
    std::memcpy(&msgIdBE, envelope + 2, sizeof(msgIdBE));
    const uint32_t messageId = ntohl(msgIdBE);

    // Step 3: read the payload.
    std::string payload(payloadLen, '\0');
    if (payloadLen > 0)
    {
        const auto payStatus = readExact(fd, reinterpret_cast<uint8_t*>(payload.data()), payloadLen);
        if (payStatus != FrameReadStatus::OK)
        {
            return {payStatus, {}, 0, {}};
        }
    }

    return {FrameReadStatus::OK, msgType, messageId, std::move(payload)};
}

bool MessageSerializer::sendAll(int fd, const uint8_t* data, std::size_t size)
{
    std::size_t totalSent = 0;

    while (totalSent < size)
    {
        const auto sent = ::send(fd, data + totalSent, size - totalSent, MSG_NOSIGNAL);

        if (sent < 0)
        {
            // Transient interruption by a POSIX signal — safe to retry.
            if (errno == EINTR)
            {
                continue;
            }
            std::cerr << "[ERROR] messageSerializer: send() failed: " << std::system_category().message(errno) << "\n";
            return false;
        }

        totalSent += static_cast<std::size_t>(sent);
    }

    return true;
}
