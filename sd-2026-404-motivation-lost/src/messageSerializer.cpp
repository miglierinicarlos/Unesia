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
    constexpr uint32_t MAX_FRAME_PAYLOAD = 8U * 1024U * 1024U;

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

std::string MessageSerializer::buildAnalyzeGraphPayload(const AnalyzeGraphPayloadData& data)
{
    nlohmann::json j;
    j["algorithm"] = data.algorithm;

    if (data.sourceNode.has_value())
    {
        j["source_node"] = data.sourceNode.value();
    }

    // Serialize adjacency list as a JSON object: { "nodeId": ["neighbor1", ...], ... }
    nlohmann::json graphJson = nlohmann::json::object();
    for (const auto& [nodeId, neighbours] : data.graph)
    {
        graphJson[nodeId] = neighbours;
    }
    j["graph"] = graphJson;
    j["trace_id"] = data.traceId;

    return j.dump();
}

std::string MessageSerializer::buildAnalyzeResultPayload(const AnalyzeResultPayloadData& data)
{
    nlohmann::json algorithmsJson = nlohmann::json::array();
    for (const auto& entry : data.algorithms)
    {
        nlohmann::json item;
        item["algorithm"] = entry.algorithm;
        // Embed pre-serialized result JSON — parse once to avoid double-encoding.
        item["result"] = nlohmann::json::parse(entry.resultJson);
        algorithmsJson.push_back(std::move(item));
    }

    nlohmann::json j;
    j["algorithms"] = std::move(algorithmsJson);
    j["graph_size"] = {{"nodes", data.graphSize.nodes}, {"edges", data.graphSize.edges}};
    j["processing_time_ms"] = data.processingTimeMs;
    j["thread_count"] = data.threadCount;

    return j.dump();
}

std::optional<MessageSerializer::AnalyzeGraphPayloadData>
MessageSerializer::parseAnalyzeGraphPayload(const std::string& payload)
{
    try
    {
        const auto j = nlohmann::json::parse(payload);

        if (!j.contains("algorithm") || !j.contains("graph") || !j.contains("trace_id"))
        {
            return std::nullopt;
        }

        AnalyzeGraphPayloadData data;
        data.algorithm = j.at("algorithm").get<std::string>();
        data.traceId = j.at("trace_id").get<std::string>();

        if (j.contains("source_node") && !j["source_node"].is_null())
        {
            data.sourceNode = j.at("source_node").get<uint32_t>();
        }

        const auto& graphJson = j.at("graph");
        for (const auto& [nodeId, neighbours] : graphJson.items())
        {
            data.graph[nodeId] = neighbours.get<std::vector<std::string>>();
        }

        return data;
    }
    catch (const nlohmann::json::exception&)
    {
        return std::nullopt;
    }
}

std::optional<MessageSerializer::AnalyzeResultPayloadData>
MessageSerializer::parseAnalyzeResultPayload(const std::string& payload)
{
    try
    {
        const auto j = nlohmann::json::parse(payload);

        if (!j.contains("algorithms") || !j.contains("graph_size") || !j.contains("processing_time_ms") ||
            !j.contains("thread_count"))
        {
            return std::nullopt;
        }

        AnalyzeResultPayloadData data;
        data.processingTimeMs = j.at("processing_time_ms").get<uint32_t>();
        data.threadCount = j.at("thread_count").get<uint32_t>();

        const auto& sizeJson = j.at("graph_size");
        data.graphSize.nodes = sizeJson.at("nodes").get<uint32_t>();
        data.graphSize.edges = sizeJson.at("edges").get<uint32_t>();

        for (const auto& item : j.at("algorithms"))
        {
            AlgorithmResultData entry;
            entry.algorithm = item.at("algorithm").get<std::string>();
            entry.resultJson = item.at("result").dump();
            data.algorithms.push_back(std::move(entry));
        }

        return data;
    }
    catch (const nlohmann::json::exception&)
    {
        return std::nullopt;
    }
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
