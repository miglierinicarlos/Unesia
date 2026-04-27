#include "analyzeHandler.hpp"
#include "otelScope.hpp"

#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/time.h>

namespace
{
    constexpr const char* OPERATION_NAME = "analyze_graph";
} // namespace

AnalyzeHandler::AnalyzeHandler(NodeRegistry& registry, const ServerConfig& config, Logger& logger)
    : m_registry(registry)
    , m_config(config)
    , m_logger(logger)
{
}

void AnalyzeHandler::handle(uint32_t messageId,
                            const std::string& jsonPayload,
                            const std::string& sessionId,
                            const SendFn& sender)
{
    /* Parse payload to extract trace context before creating the dispatch span */
    const auto parsed = nlohmann::json::parse(jsonPayload, nullptr, false);
    if (parsed.is_discarded())
    {
        OtelScope errSpan(OPERATION_NAME);
        errSpan.setAttribute("operation.name", "ANALYZE_DISPATCH");
        errSpan.setAttribute("session.id", sessionId);
        errSpan.setAttribute("result", "ERROR");
        errSpan.setAttribute("error.code", "ERR_MALFORMED");
        errSpan.recordError("Invalid JSON in ANALYZE_GRAPH payload");

        const auto errPayload = MessageSerializer::buildErrorPayload(
            {messageId, MessageSerializer::ProtocolErrorCode::ERR_MALFORMED, "Malformed ANALYZE_GRAPH payload"});
        const auto frame =
            MessageSerializer::buildFrame(MessageSerializer::MessageType::ERROR_MSG, messageId, errPayload);
        sender(frame.data(), frame.size());
        m_logger.log(Logger::Level::WARN, "AnalyzeHandler: malformed JSON payload — returned ERR_MALFORMED");
        return;
    }

    const std::string clientTraceId = parsed.value("trace_id", "");
    const int64_t graphSize = (parsed.contains("adjacency") && parsed["adjacency"].is_array())
                                  ? static_cast<int64_t>(parsed["adjacency"].size())
                                  : static_cast<int64_t>(0);

    OtelScope span(OPERATION_NAME, OtelTraceContext {clientTraceId});
    span.setAttribute("operation.name", "ANALYZE_DISPATCH");
    span.setAttribute("session.id", sessionId);

    /* Inject trace_id into the forwarded payload so the engine continues the same trace */
    nlohmann::json enrichedJson = parsed;
    enrichedJson["trace_id"] = span.traceId();
    const std::string enrichedPayload = enrichedJson.dump();

    /* Step 1: look up engine by configured node ID */
    const auto engineEntry = m_registry.queryNode(m_config.m_hpcNodeId);

    if (!engineEntry.has_value() || engineEntry->status == NodeRegistry::NodeStatus::OFFLINE)
    {
        span.setAttribute("result", "ERROR");
        span.setAttribute("error.code", "ERR_SERVICE_UNAVAIL");
        span.recordError("HPC engine absent or OFFLINE");

        const auto errPayload = MessageSerializer::buildErrorPayload(
            {messageId, MessageSerializer::ProtocolErrorCode::ERR_SERVICE_UNAVAIL, "HPC engine unavailable"});
        const auto frame =
            MessageSerializer::buildFrame(MessageSerializer::MessageType::ERROR_MSG, messageId, errPayload);
        sender(frame.data(), frame.size());

        m_logger.log(Logger::Level::WARN, "AnalyzeHandler: engine absent or OFFLINE — returned ERR_SERVICE_UNAVAIL");
        return;
    }

    span.setAttribute("engine.node_id", m_config.m_hpcNodeId);
    span.setAttribute("graph_size", graphSize);

    /* Step 2: acquire exclusive fd access (serializes concurrent dispatches) */
    auto access = m_registry.acquireEngineFd(m_config.m_hpcNodeId);

    if (!access.valid)
    {
        span.setAttribute("result", "ERROR");
        span.setAttribute("error.code", "ERR_SERVICE_UNAVAIL");
        span.recordError("HPC engine fd not available");

        const auto errPayload = MessageSerializer::buildErrorPayload(
            {messageId, MessageSerializer::ProtocolErrorCode::ERR_SERVICE_UNAVAIL, "HPC engine fd not available"});
        const auto frame =
            MessageSerializer::buildFrame(MessageSerializer::MessageType::ERROR_MSG, messageId, errPayload);
        sender(frame.data(), frame.size());

        m_logger.log(Logger::Level::WARN, "AnalyzeHandler: engine fd unavailable — returned ERR_SERVICE_UNAVAIL");
        return;
    }

    /* Step 3: set receive timeout on engine fd */
    struct timeval tv
    {
    };
    tv.tv_sec = static_cast<time_t>(m_config.m_hpcTimeoutSecs);
    tv.tv_usec = 0;
    if (::setsockopt(access.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
    {
        span.setAttribute("result", "ERROR");
        span.setAttribute("error.code", "ERR_INTERNAL");
        span.recordError("Failed to set SO_RCVTIMEO on engine fd");

        const auto errPayload = MessageSerializer::buildErrorPayload(
            {messageId, MessageSerializer::ProtocolErrorCode::ERR_INTERNAL, "Internal configuration error"});
        const auto frame =
            MessageSerializer::buildFrame(MessageSerializer::MessageType::ERROR_MSG, messageId, errPayload);
        sender(frame.data(), frame.size());

        m_logger.log(Logger::Level::ERROR, "AnalyzeHandler: setsockopt SO_RCVTIMEO failed");
        return;
    }

    /* Step 4: forward ANALYZE_GRAPH frame to engine (payload carries trace_id for context propagation) */
    const auto requestFrame =
        MessageSerializer::buildFrame(MessageSerializer::MessageType::ANALYZE_GRAPH, messageId, enrichedPayload);
    if (!MessageSerializer::sendAll(access.fd, requestFrame.data(), requestFrame.size()))
    {
        span.setAttribute("result", "ERROR");
        span.setAttribute("error.code", "ERR_SERVICE_UNAVAIL");
        span.recordError("Failed to send ANALYZE_GRAPH to engine");

        const auto errPayload = MessageSerializer::buildErrorPayload(
            {messageId, MessageSerializer::ProtocolErrorCode::ERR_SERVICE_UNAVAIL, "Engine send failed"});
        const auto frame =
            MessageSerializer::buildFrame(MessageSerializer::MessageType::ERROR_MSG, messageId, errPayload);
        sender(frame.data(), frame.size());

        m_logger.log(Logger::Level::WARN, "AnalyzeHandler: sendAll to engine failed — returned ERR_SERVICE_UNAVAIL");
        return;
    }

    /* Step 5: read ANALYZE_RESULT from engine (SO_RCVTIMEO guards this read) */
    const auto result = MessageSerializer::readFrame(access.fd);

    if (result.status == MessageSerializer::FrameReadStatus::TIMEOUT)
    {
        span.setAttribute("result", "ERROR");
        span.setAttribute("error.code", "ERR_TIMEOUT");
        span.recordError("Engine did not respond within timeout");

        const auto errPayload = MessageSerializer::buildErrorPayload(
            {messageId, MessageSerializer::ProtocolErrorCode::ERR_TIMEOUT, "HPC engine response timeout"});
        const auto frame =
            MessageSerializer::buildFrame(MessageSerializer::MessageType::ERROR_MSG, messageId, errPayload);
        sender(frame.data(), frame.size());

        m_logger.log(Logger::Level::WARN, "AnalyzeHandler: engine response timeout — returned ERR_TIMEOUT");
        return;
    }

    if (result.status != MessageSerializer::FrameReadStatus::OK)
    {
        span.setAttribute("result", "ERROR");
        span.setAttribute("error.code", "ERR_SERVICE_UNAVAIL");
        span.recordError("I/O error reading engine response");

        const auto errPayload = MessageSerializer::buildErrorPayload(
            {messageId, MessageSerializer::ProtocolErrorCode::ERR_SERVICE_UNAVAIL, "Engine I/O error"});
        const auto frame =
            MessageSerializer::buildFrame(MessageSerializer::MessageType::ERROR_MSG, messageId, errPayload);
        sender(frame.data(), frame.size());

        m_logger.log(Logger::Level::WARN, "AnalyzeHandler: engine I/O error — returned ERR_SERVICE_UNAVAIL");
        return;
    }

    if (result.messageType != MessageSerializer::MessageType::ANALYZE_RESULT)
    {
        span.setAttribute("result", "ERROR");
        span.setAttribute("error.code", "ERR_INTERNAL");
        span.recordError("Unexpected message type from engine");

        const auto errPayload = MessageSerializer::buildErrorPayload(
            {messageId, MessageSerializer::ProtocolErrorCode::ERR_INTERNAL, "Unexpected engine response type"});
        const auto frame =
            MessageSerializer::buildFrame(MessageSerializer::MessageType::ERROR_MSG, messageId, errPayload);
        sender(frame.data(), frame.size());

        m_logger.log(Logger::Level::ERROR,
                     "AnalyzeHandler: unexpected response type from engine — returned ERR_INTERNAL");
        return;
    }

    /* Step 6: forward ANALYZE_RESULT to originating client */
    const auto responseFrame =
        MessageSerializer::buildFrame(MessageSerializer::MessageType::ANALYZE_RESULT, messageId, result.payload);
    sender(responseFrame.data(), responseFrame.size());

    span.setAttribute("result", "OK");
    m_logger.log(Logger::Level::INFO, "AnalyzeHandler: ANALYZE_RESULT forwarded to client");
}
