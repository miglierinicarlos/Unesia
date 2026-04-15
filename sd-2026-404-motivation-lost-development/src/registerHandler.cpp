#include "registerHandler.hpp"
#include "otelScope.hpp"

#include <chrono>
#include <nlohmann/json.hpp>

namespace
{
    constexpr const char* FIELD_NODE_ID = "node_id";
    constexpr const char* FIELD_BUNKER_NAME = "bunker_name";
    constexpr const char* FIELD_IP_ADDRESS = "ip_address";
    constexpr const char* FIELD_CAPACITY = "capacity";
} // namespace

RegisterHandler::RegisterHandler(NodeRegistry& registry, Logger& logger)
    : m_registry(registry)
    , m_logger(logger)
{
}

void RegisterHandler::handle(uint32_t messageId,
                             const std::string& jsonPayload,
                             const std::string& sessionId,
                             const SendFn& sender)
{
    const auto startTime = std::chrono::steady_clock::now();
    OtelScope span("register_node");
    span.setAttribute("operation.name", "REGISTER");
    span.setAttribute("session.id", sessionId);

    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(jsonPayload);
    }
    catch (const nlohmann::json::exception&)
    {
        span.setAttribute("result", "ERROR");
        span.setAttribute("error.code", "ERR_MALFORMED");
        span.recordError("Invalid JSON payload");

        auto payload = MessageSerializer::buildErrorPayload(
            {messageId, MessageSerializer::ProtocolErrorCode::ERR_MALFORMED, "Invalid JSON payload"});
        auto frame = MessageSerializer::buildFrame(MessageSerializer::MessageType::ERROR_MSG, messageId, payload);
        sender(frame.data(), frame.size());

        m_logger.logNodeRegisterError({span.traceId(), span.spanId()}, {"", sessionId, "ERR_MALFORMED"});
        return;
    }

    /* Validate required fields and types */
    if (!j.contains(FIELD_NODE_ID) || !j[FIELD_NODE_ID].is_string() || j[FIELD_NODE_ID].get<std::string>().empty() ||
        !j.contains(FIELD_BUNKER_NAME) || !j[FIELD_BUNKER_NAME].is_string() || !j.contains(FIELD_IP_ADDRESS) ||
        !j[FIELD_IP_ADDRESS].is_string() || !j.contains(FIELD_CAPACITY) || !j[FIELD_CAPACITY].is_number_unsigned())
    {
        span.setAttribute("result", "ERROR");
        span.setAttribute("error.code", "ERR_MALFORMED");
        span.recordError("Missing or invalid required fields");

        auto payload = MessageSerializer::buildErrorPayload(
            {messageId,
             MessageSerializer::ProtocolErrorCode::ERR_MALFORMED,
             "Missing or invalid required fields: node_id, bunker_name, ip_address, capacity"});
        auto frame = MessageSerializer::buildFrame(MessageSerializer::MessageType::ERROR_MSG, messageId, payload);
        sender(frame.data(), frame.size());

        m_logger.logNodeRegisterError({span.traceId(), span.spanId()}, {"", sessionId, "ERR_MALFORMED"});
        return;
    }

    const std::string nodeId = j[FIELD_NODE_ID].get<std::string>();
    span.setAttribute("node.id", nodeId);

    NodeRegistry::NodeEntry entry;
    entry.node_id = nodeId;
    entry.bunker_name = j[FIELD_BUNKER_NAME].get<std::string>();
    entry.ip_address = j[FIELD_IP_ADDRESS].get<std::string>();
    entry.capacity = j[FIELD_CAPACITY].get<uint64_t>();

    const auto result = m_registry.registerNode(std::move(entry));

    const auto durationMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
    span.setAttribute("duration_ms", durationMs);

    if (result == NodeRegistry::RegisterResult::OK)
    {
        span.setAttribute("result", "OK");

        auto payload = MessageSerializer::buildAckPayload({messageId, nodeId});
        auto frame = MessageSerializer::buildFrame(MessageSerializer::MessageType::ACK, messageId, payload);
        sender(frame.data(), frame.size());

        m_logger.logNodeRegistered({span.traceId(), span.spanId()}, {nodeId, sessionId});
    }
    else
    {
        span.setAttribute("result", "ERROR");
        span.setAttribute("error.code", "ERR_DUPLICATE");
        span.recordError("Node " + nodeId + " already registered");

        auto payload = MessageSerializer::buildErrorPayload(
            {messageId, MessageSerializer::ProtocolErrorCode::ERR_DUPLICATE, "Node " + nodeId + " already registered"});
        auto frame = MessageSerializer::buildFrame(MessageSerializer::MessageType::ERROR_MSG, messageId, payload);
        sender(frame.data(), frame.size());

        m_logger.logNodeRegisterError({span.traceId(), span.spanId()}, {nodeId, sessionId, "ERR_DUPLICATE"});
    }
}
