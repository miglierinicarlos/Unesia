#include "queryHandler.hpp"
#include "otelScope.hpp"

#include <array>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace
{
    constexpr const char* FIELD_NODE_ID = "node_id";
    constexpr const char* FIELD_NODES = "nodes";
} // namespace

QueryHandler::QueryHandler(NodeRegistry& registry, Logger& logger)
    : m_registry(registry)
    , m_logger(logger)
{
}

void QueryHandler::handle(MessageSerializer::MessageType messageType,
                          uint32_t messageId,
                          const std::string& jsonPayload,
                          const std::string& sessionId,
                          const SendFn& sender)
{
    if (messageType == MessageSerializer::MessageType::QUERY_NODE)
    {
        handleQueryNode(messageId, jsonPayload, sessionId, sender);
    }
    else
    {
        handleListNodes(messageId, sessionId, sender);
    }
}

void QueryHandler::handleQueryNode(uint32_t messageId,
                                   const std::string& jsonPayload,
                                   const std::string& sessionId,
                                   const SendFn& sender)
{
    const auto startTime = std::chrono::steady_clock::now();
    OtelScope span("query_node");
    span.setAttribute("operation.name", "QUERY_NODE");
    span.setAttribute("session.id", sessionId);

    auto setDuration = [&]()
    {
        const auto durationMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
        span.setAttribute("duration_ms", durationMs);
    };

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
        setDuration();

        auto payload = MessageSerializer::buildErrorPayload(
            {messageId, MessageSerializer::ProtocolErrorCode::ERR_MALFORMED, "Invalid JSON payload"});
        auto frame = MessageSerializer::buildFrame(MessageSerializer::MessageType::ERROR_MSG, messageId, payload);
        sender(frame.data(), frame.size());

        m_logger.logNodeQueried({span.traceId(), span.spanId()}, {"", sessionId, "ERR_MALFORMED"});
        return;
    }

    if (!j.contains(FIELD_NODE_ID) || !j[FIELD_NODE_ID].is_string())
    {
        span.setAttribute("result", "ERROR");
        span.setAttribute("error.code", "ERR_MALFORMED");
        span.recordError("Missing or invalid field: node_id");
        setDuration();

        auto payload = MessageSerializer::buildErrorPayload(
            {messageId, MessageSerializer::ProtocolErrorCode::ERR_MALFORMED, "Missing or invalid field: node_id"});
        auto frame = MessageSerializer::buildFrame(MessageSerializer::MessageType::ERROR_MSG, messageId, payload);
        sender(frame.data(), frame.size());

        m_logger.logNodeQueried({span.traceId(), span.spanId()}, {"", sessionId, "ERR_MALFORMED"});
        return;
    }

    const std::string nodeId = j[FIELD_NODE_ID].get<std::string>();
    span.setAttribute("node.id", nodeId);

    const auto entry = m_registry.queryNode(nodeId);

    if (!entry.has_value())
    {
        span.setAttribute("result", "ERROR");
        span.setAttribute("error.code", "ERR_NOT_FOUND");
        span.recordError("Node " + nodeId + " not found");
        setDuration();

        auto payload = MessageSerializer::buildErrorPayload(
            {messageId, MessageSerializer::ProtocolErrorCode::ERR_NOT_FOUND, "Node " + nodeId + " not found"});
        auto frame = MessageSerializer::buildFrame(MessageSerializer::MessageType::ERROR_MSG, messageId, payload);
        sender(frame.data(), frame.size());

        m_logger.logNodeQueried({span.traceId(), span.spanId()}, {nodeId, sessionId, "ERR_NOT_FOUND"});
        return;
    }

    span.setAttribute("result", "OK");
    setDuration();

    nlohmann::json response;
    response["ref_message_id"] = messageId;
    response[FIELD_NODE_ID] = entry->node_id;
    response["bunker_name"] = entry->bunker_name;
    response["ip_address"] = entry->ip_address;
    response["capacity"] = entry->capacity;
    response["status"] = statusToString(entry->status);
    if (entry->status == NodeRegistry::NodeStatus::OFFLINE)
    {
        response["last_seen_at"] = toIso8601(entry->last_seen_at);
    }

    auto frame = MessageSerializer::buildFrame(MessageSerializer::MessageType::ACK, messageId, response.dump());
    sender(frame.data(), frame.size());

    m_logger.logNodeQueried({span.traceId(), span.spanId()}, {nodeId, sessionId, "OK"});
}

void QueryHandler::handleListNodes(uint32_t messageId, const std::string& sessionId, const SendFn& sender)
{
    const auto startTime = std::chrono::steady_clock::now();
    OtelScope span("list_nodes");
    span.setAttribute("operation.name", "LIST_NODES");
    span.setAttribute("session.id", sessionId);

    const auto entries = m_registry.listNodes();

    const auto durationMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();

    span.setAttribute("node_count", static_cast<int64_t>(entries.size()));
    span.setAttribute("duration_ms", durationMs);
    span.setAttribute("result", "OK");

    nlohmann::json nodesArray = nlohmann::json::array();
    for (const auto& entry : entries)
    {
        nodesArray.push_back(entryToJson(entry));
    }

    nlohmann::json response;
    response["ref_message_id"] = messageId;
    response[FIELD_NODES] = nodesArray;

    auto frame = MessageSerializer::buildFrame(MessageSerializer::MessageType::ACK, messageId, response.dump());
    sender(frame.data(), frame.size());

    m_logger.logNodesListed({span.traceId(), span.spanId()}, {sessionId, entries.size()});
}

nlohmann::json QueryHandler::entryToJson(const NodeRegistry::NodeEntry& entry)
{
    nlohmann::json j;
    j[FIELD_NODE_ID] = entry.node_id;
    j["bunker_name"] = entry.bunker_name;
    j["ip_address"] = entry.ip_address;
    j["capacity"] = entry.capacity;
    j["status"] = statusToString(entry.status);
    if (entry.status == NodeRegistry::NodeStatus::OFFLINE)
    {
        j["last_seen_at"] = toIso8601(entry.last_seen_at);
    }
    return j;
}

const char* QueryHandler::statusToString(NodeRegistry::NodeStatus status)
{
    return status == NodeRegistry::NodeStatus::ONLINE ? "ONLINE" : "OFFLINE";
}

std::string QueryHandler::toIso8601(std::chrono::system_clock::time_point tp)
{
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tmUtc {};
    ::gmtime_r(&t, &tmUtc);

    std::ostringstream oss;
    oss << std::put_time(&tmUtc, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}
