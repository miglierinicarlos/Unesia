#include "nodeRegistry.hpp"
#include "logger.hpp"
#include "messageSerializer.hpp"
#include "queryHandler.hpp"
#include "registerHandler.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <functional>
#include <thread>
#include <vector>

using Result = NodeRegistry::RegisterResult;
using Status = NodeRegistry::NodeStatus;

/* Helper: builds a NodeEntry with all fields populated */
static NodeRegistry::NodeEntry makeEntry(const std::string& node_id,
                                         const std::string& bunker = "Bunker-A",
                                         const std::string& ip = "10.0.0.1",
                                         uint64_t cap = 100)
{
    return {node_id, bunker, ip, cap};
}

// =============================================================================
// NodeRegistryTest.registerNodeSuccess
// =============================================================================
TEST(NodeRegistryTest, registerNodeSuccess)
{
    NodeRegistry reg;
    const auto result = reg.registerNode(makeEntry("vault-01"));
    EXPECT_EQ(result, Result::OK);
    EXPECT_TRUE(reg.contains("vault-01"));
}

// =============================================================================
// NodeRegistryTest.registerDuplicateOnlineReturnsError
// =============================================================================
TEST(NodeRegistryTest, registerDuplicateOnlineReturnsError)
{
    NodeRegistry reg;
    reg.registerNode(makeEntry("vault-01", "Original", "1.1.1.1", 50));

    const auto result = reg.registerNode(makeEntry("vault-01", "Duplicate", "2.2.2.2", 99));
    EXPECT_EQ(result, Result::ERR_DUPLICATE);

    /* Existing entry must be unchanged */
    const auto entry = reg.queryNode("vault-01");
    ASSERT_TRUE(entry.has_value());
    if (!entry)
        return; // Clang-tidy dataflow guard

    EXPECT_EQ(entry->bunker_name, "Original");
    EXPECT_EQ(entry->ip_address, "1.1.1.1");
    EXPECT_EQ(entry->capacity, 50u);
    EXPECT_EQ(entry->status, Status::ONLINE);
}

// =============================================================================
// NodeRegistryTest.queryExistingNodeReturnsAllFields
// =============================================================================
TEST(NodeRegistryTest, queryExistingNodeReturnsAllFields)
{
    NodeRegistry reg;
    reg.registerNode(makeEntry("vault-13", "Vault Tec HQ", "192.168.1.1", 500));

    const auto entry = reg.queryNode("vault-13");
    ASSERT_TRUE(entry.has_value());
    if (!entry)
        return; // Clang-tidy dataflow guard

    EXPECT_EQ(entry->node_id, "vault-13");
    EXPECT_EQ(entry->bunker_name, "Vault Tec HQ");
    EXPECT_EQ(entry->ip_address, "192.168.1.1");
    EXPECT_EQ(entry->capacity, 500u);
    EXPECT_EQ(entry->status, Status::ONLINE);
}

// =============================================================================
// NodeRegistryTest.queryNonExistentReturnsNotFound
// =============================================================================
TEST(NodeRegistryTest, queryNonExistentReturnsNotFound)
{
    NodeRegistry reg;
    const auto entry = reg.queryNode("ghost-node");
    EXPECT_FALSE(entry.has_value());
}

// =============================================================================
// NodeRegistryTest.listNodesReturnsAll
// =============================================================================
TEST(NodeRegistryTest, listNodesReturnsAll)
{
    NodeRegistry reg;
    reg.registerNode(makeEntry("vault-01"));
    reg.registerNode(makeEntry("vault-02"));
    reg.registerNode(makeEntry("vault-03"));

    const auto snapshot = reg.listNodes();
    EXPECT_EQ(snapshot.size(), 3u);

    /* All node_ids must be present */
    std::vector<std::string> ids;
    ids.reserve(snapshot.size());
    for (const auto& e : snapshot) ids.push_back(e.node_id);

    EXPECT_NE(std::find(ids.begin(), ids.end(), "vault-01"), ids.end());
    EXPECT_NE(std::find(ids.begin(), ids.end(), "vault-02"), ids.end());
    EXPECT_NE(std::find(ids.begin(), ids.end(), "vault-03"), ids.end());
}

// =============================================================================
// NodeRegistryTest.registrySurvivesConnectionClose
// =============================================================================
TEST(NodeRegistryTest, registrySurvivesConnectionClose)
{
    NodeRegistry reg;
    reg.registerNode(makeEntry("vault-01"));

    /* Simulate connection close */
    reg.setOffline("vault-01");

    /* Entry must still exist */
    EXPECT_TRUE(reg.contains("vault-01"));
    EXPECT_TRUE(reg.isOffline("vault-01"));

    const auto entry = reg.queryNode("vault-01");
    ASSERT_TRUE(entry.has_value());
    if (!entry)
        return; // Clang-tidy dataflow guard

    EXPECT_EQ(entry->node_id, "vault-01");
}

// =============================================================================
// NodeRegistryTest.reregistrationOfOfflineGoesOnline
// =============================================================================
TEST(NodeRegistryTest, reregistrationOfOfflineGoesOnline)
{
    NodeRegistry reg;
    reg.registerNode(makeEntry("vault-01", "Old Name", "1.1.1.1", 10));
    reg.setOffline("vault-01");
    ASSERT_TRUE(reg.isOffline("vault-01"));

    /* Re-register with updated metadata */
    const auto result = reg.registerNode(makeEntry("vault-01", "New Name", "2.2.2.2", 99));
    EXPECT_EQ(result, Result::OK);
    EXPECT_FALSE(reg.isOffline("vault-01"));

    const auto entry = reg.queryNode("vault-01");
    ASSERT_TRUE(entry.has_value());
    if (!entry)
        return; // Clang-tidy dataflow guard

    EXPECT_EQ(entry->status, Status::ONLINE);
    EXPECT_EQ(entry->bunker_name, "New Name");
    EXPECT_EQ(entry->ip_address, "2.2.2.2");
    EXPECT_EQ(entry->capacity, 99u);
}

// =============================================================================
// NodeRegistryTest.concurrentReadsNeverBlock
// =============================================================================
TEST(NodeRegistryTest, concurrentReadsNeverBlock)
{
    static const int READER_COUNT = 16;

    NodeRegistry reg;
    reg.registerNode(makeEntry("vault-01"));
    reg.registerNode(makeEntry("vault-02"));

    std::vector<std::thread> threads;
    threads.reserve(READER_COUNT);

    for (int i = 0; i < READER_COUNT; ++i)
    {
        threads.emplace_back(
            [&reg]()
            {
                const auto e1 = reg.queryNode("vault-01");
                const auto e2 = reg.queryNode("vault-02");
                const auto all = reg.listNodes();
                EXPECT_TRUE(e1.has_value());
                EXPECT_TRUE(e2.has_value());
                EXPECT_EQ(all.size(), 2u);
            });
    }

    for (auto& t : threads) t.join();
}

// =============================================================================
// NodeRegistryTest.setOfflineUpdatesLastSeenAt
// =============================================================================
TEST(NodeRegistryTest, setOfflineUpdatesLastSeenAt)
{
    NodeRegistry reg;
    reg.registerNode(makeEntry("vault-01"));

    const auto before = std::chrono::system_clock::now();
    reg.setOffline("vault-01");
    const auto after = std::chrono::system_clock::now();

    const auto entry = reg.queryNode("vault-01");
    ASSERT_TRUE(entry.has_value());
    if (!entry)
        return; // Clang-tidy dataflow guard

    EXPECT_GE(entry->last_seen_at, before);
    EXPECT_LE(entry->last_seen_at, after);
}

// =============================================================================
// NodeRegistryTest.setOfflineUnknownNodeIsNoOp
// =============================================================================
TEST(NodeRegistryTest, setOfflineUnknownNodeIsNoOp)
{
    NodeRegistry reg;
    /* Must not crash */
    reg.setOffline("ghost-node");
    reg.setOffline("");
    EXPECT_FALSE(reg.contains("ghost-node"));
}

// =============================================================================
// Handler-level tests
// =============================================================================

namespace
{
    constexpr uint32_t MSG_ID = 42;
    constexpr const char* SESSION_ID = "sess-test";

    using SendFn = std::function<bool(const uint8_t*, std::size_t)>;

    static MessageSerializer::FrameReadResult captureFrame(const std::function<void(const SendFn&)>& action)
    {
        std::array<int, 2> sv = {-1, -1};
        // NOLINTNEXTLINE(android-cloexec-socketpair)
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()) != 0)
        {
            return {MessageSerializer::FrameReadStatus::IO_ERROR, {}, 0, {}};
        }

        std::vector<uint8_t> buf;
        SendFn sender = [&buf](const uint8_t* data, std::size_t size) -> bool
        {
            buf.insert(buf.end(), data, data + size);
            return true;
        };

        action(sender);

        const ssize_t written = ::send(sv[1], buf.data(), buf.size(), MSG_NOSIGNAL);
        (void)written;
        ::shutdown(sv[1], SHUT_WR);

        auto result = MessageSerializer::readFrame(sv[0]);
        ::close(sv[0]);
        ::close(sv[1]);
        return result;
    }
} // namespace

// =============================================================================
// RegisterHandlerTest.successfulRegistrationSendsAck
// =============================================================================
TEST(RegisterHandlerTest, successfulRegistrationSendsAck)
{
    NodeRegistry reg;
    Logger logger(Logger::Level::NONE);
    RegisterHandler handler(reg, logger);

    const std::string payload =
        R"({"node_id":"vault-01","bunker_name":"Bunker A","ip_address":"10.0.0.1","capacity":100})";

    const auto frame = captureFrame([&](const SendFn& sender) { handler.handle(MSG_ID, payload, SESSION_ID, sender); });

    ASSERT_EQ(frame.status, MessageSerializer::FrameReadStatus::OK);
    EXPECT_EQ(frame.messageType, MessageSerializer::MessageType::ACK);
    EXPECT_EQ(frame.messageId, MSG_ID);

    const auto j = nlohmann::json::parse(frame.payload);
    EXPECT_EQ(j["node_id"], "vault-01");
    EXPECT_EQ(j["ref_message_id"], MSG_ID);

    EXPECT_TRUE(reg.contains("vault-01"));
}

// =============================================================================
// RegisterHandlerTest.duplicateRegistrationSendsErrDuplicate
// =============================================================================
TEST(RegisterHandlerTest, duplicateRegistrationSendsErrDuplicate)
{
    NodeRegistry reg;
    reg.registerNode(makeEntry("vault-01", "Original", "1.1.1.1", 50));
    Logger logger(Logger::Level::NONE);
    RegisterHandler handler(reg, logger);

    const std::string payload = R"({"node_id":"vault-01","bunker_name":"Dup","ip_address":"2.2.2.2","capacity":99})";

    const auto frame = captureFrame([&](const SendFn& sender) { handler.handle(MSG_ID, payload, SESSION_ID, sender); });

    ASSERT_EQ(frame.status, MessageSerializer::FrameReadStatus::OK);
    EXPECT_EQ(frame.messageType, MessageSerializer::MessageType::ERROR_MSG);

    const auto j = nlohmann::json::parse(frame.payload);
    EXPECT_EQ(j["error_code"], static_cast<uint8_t>(MessageSerializer::ProtocolErrorCode::ERR_DUPLICATE));

    // Original entry must be untouched (AC3)
    const auto entry = reg.queryNode("vault-01");
    ASSERT_TRUE(entry.has_value());
    if (!entry)
        return; // Clang-tidy dataflow guard

    EXPECT_EQ(entry->bunker_name, "Original");
    EXPECT_EQ(entry->capacity, 50u);
}

// =============================================================================
// RegisterHandlerTest.malformedJsonSendsErrMalformed
// =============================================================================
TEST(RegisterHandlerTest, malformedJsonSendsErrMalformed)
{
    NodeRegistry reg;
    Logger logger(Logger::Level::NONE);
    RegisterHandler handler(reg, logger);

    const auto frame =
        captureFrame([&](const SendFn& sender) { handler.handle(MSG_ID, "not-json{{{", SESSION_ID, sender); });

    ASSERT_EQ(frame.status, MessageSerializer::FrameReadStatus::OK);
    EXPECT_EQ(frame.messageType, MessageSerializer::MessageType::ERROR_MSG);

    const auto j = nlohmann::json::parse(frame.payload);
    EXPECT_EQ(j["error_code"], static_cast<uint8_t>(MessageSerializer::ProtocolErrorCode::ERR_MALFORMED));
}

// =============================================================================
// RegisterHandlerTest.missingRequiredFieldSendsErrMalformed
// =============================================================================
TEST(RegisterHandlerTest, missingRequiredFieldSendsErrMalformed)
{
    NodeRegistry reg;
    Logger logger(Logger::Level::NONE);
    RegisterHandler handler(reg, logger);

    // capacity field is missing
    const std::string payload = R"({"node_id":"vault-01","bunker_name":"B","ip_address":"1.1.1.1"})";

    const auto frame = captureFrame([&](const SendFn& sender) { handler.handle(MSG_ID, payload, SESSION_ID, sender); });

    ASSERT_EQ(frame.status, MessageSerializer::FrameReadStatus::OK);
    EXPECT_EQ(frame.messageType, MessageSerializer::MessageType::ERROR_MSG);

    const auto j = nlohmann::json::parse(frame.payload);
    EXPECT_EQ(j["error_code"], static_cast<uint8_t>(MessageSerializer::ProtocolErrorCode::ERR_MALFORMED));
}

// =============================================================================
// QueryHandlerTest.queryExistingNodeSendsAckWithAllFields
// =============================================================================
TEST(QueryHandlerTest, queryExistingNodeSendsAckWithAllFields)
{
    NodeRegistry reg;
    reg.registerNode(makeEntry("vault-13", "Vault Tec HQ", "192.168.1.1", 500));
    Logger logger(Logger::Level::NONE);
    QueryHandler handler(reg, logger);

    const std::string payload = R"({"node_id":"vault-13"})";

    const auto frame = captureFrame(
        [&](const SendFn& sender)
        { handler.handle(MessageSerializer::MessageType::QUERY_NODE, MSG_ID, payload, SESSION_ID, sender); });

    ASSERT_EQ(frame.status, MessageSerializer::FrameReadStatus::OK);
    EXPECT_EQ(frame.messageType, MessageSerializer::MessageType::ACK);

    const auto j = nlohmann::json::parse(frame.payload);
    EXPECT_EQ(j["node_id"], "vault-13");
    EXPECT_EQ(j["bunker_name"], "Vault Tec HQ");
    EXPECT_EQ(j["ip_address"], "192.168.1.1");
    EXPECT_EQ(j["capacity"], 500u);
    EXPECT_EQ(j["status"], "ONLINE");
}

// =============================================================================
// QueryHandlerTest.queryNonExistentNodeSendsErrNotFound
// =============================================================================
TEST(QueryHandlerTest, queryNonExistentNodeSendsErrNotFound)
{
    NodeRegistry reg;
    Logger logger(Logger::Level::NONE);
    QueryHandler handler(reg, logger);

    const std::string payload = R"({"node_id":"ghost-node"})";

    const auto frame = captureFrame(
        [&](const SendFn& sender)
        { handler.handle(MessageSerializer::MessageType::QUERY_NODE, MSG_ID, payload, SESSION_ID, sender); });

    ASSERT_EQ(frame.status, MessageSerializer::FrameReadStatus::OK);
    EXPECT_EQ(frame.messageType, MessageSerializer::MessageType::ERROR_MSG);

    const auto j = nlohmann::json::parse(frame.payload);
    EXPECT_EQ(j["error_code"], static_cast<uint8_t>(MessageSerializer::ProtocolErrorCode::ERR_NOT_FOUND));
}

// =============================================================================
// QueryHandlerTest.queryMalformedPayloadSendsErrMalformed
// =============================================================================
TEST(QueryHandlerTest, queryMalformedPayloadSendsErrMalformed)
{
    NodeRegistry reg;
    Logger logger(Logger::Level::NONE);
    QueryHandler handler(reg, logger);

    const auto frame = captureFrame(
        [&](const SendFn& sender)
        { handler.handle(MessageSerializer::MessageType::QUERY_NODE, MSG_ID, "{{bad", SESSION_ID, sender); });

    ASSERT_EQ(frame.status, MessageSerializer::FrameReadStatus::OK);
    EXPECT_EQ(frame.messageType, MessageSerializer::MessageType::ERROR_MSG);

    const auto j = nlohmann::json::parse(frame.payload);
    EXPECT_EQ(j["error_code"], static_cast<uint8_t>(MessageSerializer::ProtocolErrorCode::ERR_MALFORMED));
}

// =============================================================================
// RegisterHandlerTest.emptyNodeIdSendsErrMalformed
// =============================================================================
TEST(RegisterHandlerTest, emptyNodeIdSendsErrMalformed)
{
    NodeRegistry reg;
    Logger logger(Logger::Level::NONE);
    RegisterHandler handler(reg, logger);

    const std::string payload = R"({"node_id":"","bunker_name":"B","ip_address":"1.1.1.1","capacity":100})";

    const auto frame = captureFrame([&](const SendFn& sender) { handler.handle(MSG_ID, payload, SESSION_ID, sender); });

    ASSERT_EQ(frame.status, MessageSerializer::FrameReadStatus::OK);
    EXPECT_EQ(frame.messageType, MessageSerializer::MessageType::ERROR_MSG);

    const auto j = nlohmann::json::parse(frame.payload);
    EXPECT_EQ(j["error_code"], static_cast<uint8_t>(MessageSerializer::ProtocolErrorCode::ERR_MALFORMED));
}

// =============================================================================
// RegisterHandlerTest.wrongCapacityTypeSendsErrMalformed
// =============================================================================
TEST(RegisterHandlerTest, wrongCapacityTypeSendsErrMalformed)
{
    NodeRegistry reg;
    Logger logger(Logger::Level::NONE);
    RegisterHandler handler(reg, logger);

    // capacity is a string instead of an unsigned integer
    const std::string payload = R"({"node_id":"vault-01","bunker_name":"B","ip_address":"1.1.1.1","capacity":"100"})";

    const auto frame = captureFrame([&](const SendFn& sender) { handler.handle(MSG_ID, payload, SESSION_ID, sender); });

    ASSERT_EQ(frame.status, MessageSerializer::FrameReadStatus::OK);
    EXPECT_EQ(frame.messageType, MessageSerializer::MessageType::ERROR_MSG);

    const auto j = nlohmann::json::parse(frame.payload);
    EXPECT_EQ(j["error_code"], static_cast<uint8_t>(MessageSerializer::ProtocolErrorCode::ERR_MALFORMED));
}

// =============================================================================
// QueryHandlerTest.listNodesSendsAckWithAllRegisteredNodes
// =============================================================================
TEST(QueryHandlerTest, listNodesSendsAckWithAllRegisteredNodes)
{
    NodeRegistry reg;
    reg.registerNode(makeEntry("vault-01"));
    reg.registerNode(makeEntry("vault-02"));
    reg.registerNode(makeEntry("vault-03"));
    Logger logger(Logger::Level::NONE);
    QueryHandler handler(reg, logger);

    const auto frame =
        captureFrame([&](const SendFn& sender)
                     { handler.handle(MessageSerializer::MessageType::LIST_NODES, MSG_ID, "", SESSION_ID, sender); });

    ASSERT_EQ(frame.status, MessageSerializer::FrameReadStatus::OK);
    EXPECT_EQ(frame.messageType, MessageSerializer::MessageType::ACK);

    const auto j = nlohmann::json::parse(frame.payload);
    ASSERT_TRUE(j.contains("nodes"));
    EXPECT_EQ(j["nodes"].size(), 3u);

    std::vector<std::string> ids;
    for (const auto& node : j["nodes"])
    {
        ids.push_back(node["node_id"].get<std::string>());
    }
    EXPECT_NE(std::find(ids.begin(), ids.end(), "vault-01"), ids.end());
    EXPECT_NE(std::find(ids.begin(), ids.end(), "vault-02"), ids.end());
    EXPECT_NE(std::find(ids.begin(), ids.end(), "vault-03"), ids.end());
}

// =============================================================================
// QueryHandlerTest.listNodesEmptyRegistrySendsEmptyArray
// =============================================================================
TEST(QueryHandlerTest, listNodesEmptyRegistrySendsEmptyArray)
{
    NodeRegistry reg;
    Logger logger(Logger::Level::NONE);
    QueryHandler handler(reg, logger);

    const auto frame =
        captureFrame([&](const SendFn& sender)
                     { handler.handle(MessageSerializer::MessageType::LIST_NODES, MSG_ID, "", SESSION_ID, sender); });

    ASSERT_EQ(frame.status, MessageSerializer::FrameReadStatus::OK);
    EXPECT_EQ(frame.messageType, MessageSerializer::MessageType::ACK);

    const auto j = nlohmann::json::parse(frame.payload);
    ASSERT_TRUE(j.contains("nodes"));
    EXPECT_TRUE(j["nodes"].is_array());
    EXPECT_EQ(j["nodes"].size(), 0u);
}

// =============================================================================
// QueryHandlerTest.queryOfflineNodeSerializesStatusAndLastSeenAt
// =============================================================================
TEST(QueryHandlerTest, queryOfflineNodeSerializesStatusAndLastSeenAt)
{
    NodeRegistry reg;
    reg.registerNode(makeEntry("vault-99", "Bunker Z", "10.0.0.99", 200));
    reg.setOffline("vault-99");
    Logger logger(Logger::Level::NONE);
    QueryHandler handler(reg, logger);

    const std::string payload = R"({"node_id":"vault-99"})";

    const auto frame = captureFrame(
        [&](const SendFn& sender)
        { handler.handle(MessageSerializer::MessageType::QUERY_NODE, MSG_ID, payload, SESSION_ID, sender); });

    ASSERT_EQ(frame.status, MessageSerializer::FrameReadStatus::OK);
    EXPECT_EQ(frame.messageType, MessageSerializer::MessageType::ACK);

    const auto j = nlohmann::json::parse(frame.payload);
    EXPECT_EQ(j["status"], "OFFLINE");
    ASSERT_TRUE(j.contains("last_seen_at"));

    /* ISO-8601 format: YYYY-MM-DDTHH:MM:SSZ */
    const std::string lastSeen = j["last_seen_at"].get<std::string>();
    EXPECT_EQ(lastSeen.size(), 20u);
    EXPECT_EQ(lastSeen.back(), 'Z');
    EXPECT_EQ(lastSeen[10], 'T');
}

// =============================================================================
// NodeRegistryTest.updateLastSeenUpdatesTimestamp
// =============================================================================
TEST(NodeRegistryTest, updateLastSeenUpdatesTimestamp)
{
    NodeRegistry reg;
    reg.registerNode(makeEntry("vault-01"));

    const auto before = std::chrono::system_clock::now();
    EXPECT_TRUE(reg.updateLastSeen("vault-01"));
    const auto after = std::chrono::system_clock::now();

    const auto entry = reg.queryNode("vault-01");
    ASSERT_TRUE(entry.has_value());
    if (!entry)
        return; // Clang-tidy dataflow guard

    EXPECT_GE(entry->last_seen_at, before);
    EXPECT_LE(entry->last_seen_at, after);
}

// =============================================================================
// NodeRegistryTest.updateLastSeenOnOfflineNodeIsNoOp
// =============================================================================
TEST(NodeRegistryTest, updateLastSeenOnOfflineNodeIsNoOp)
{
    NodeRegistry reg;
    reg.registerNode(makeEntry("vault-01"));
    reg.setOffline("vault-01");

    // Guarding the optional access properly here too:
    const auto initialNode = reg.queryNode("vault-01");
    ASSERT_TRUE(initialNode.has_value());
    if (!initialNode)
        return;
    const auto lastSeenAfterOffline = initialNode->last_seen_at;

    // Small sleep so a spurious update would produce a measurably different timestamp
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(reg.updateLastSeen("vault-01"));

    const auto entry = reg.queryNode("vault-01");
    ASSERT_TRUE(entry.has_value());
    if (!entry)
        return; // Clang-tidy dataflow guard

    EXPECT_EQ(entry->last_seen_at, lastSeenAfterOffline);
}

// =============================================================================
// NodeRegistryTest.updateLastSeenUnknownNodeIsNoOp
// =============================================================================
TEST(NodeRegistryTest, updateLastSeenUnknownNodeIsNoOp)
{
    NodeRegistry reg;
    // Must not crash on unknown or empty nodeId
    EXPECT_FALSE(reg.updateLastSeen("ghost-node"));
    EXPECT_FALSE(reg.updateLastSeen(""));
    EXPECT_FALSE(reg.contains("ghost-node"));
}

// =============================================================================
// TASK 20 — ONLINE/OFFLINE transition and re-registration tests
// =============================================================================

TEST(NodeRegistryTest, offlineNodeQueryReturnsOfflineStatus)
{
    NodeRegistry reg;
    reg.registerNode(makeEntry("vault-20"));
    reg.setOffline("vault-20");

    const auto entry = reg.queryNode("vault-20");
    ASSERT_TRUE(entry.has_value());
    if (!entry)
        return; // Clang-tidy dataflow guard

    EXPECT_EQ(entry->status, NodeRegistry::NodeStatus::OFFLINE);
    EXPECT_EQ(entry->node_id, "vault-20");
}

TEST(NodeRegistryTest, reregistrationOfOfflineNodeGoesOnline)
{
    NodeRegistry reg;
    reg.registerNode(makeEntry("vault-20", "Old Name", "1.1.1.1", 10));
    reg.setOffline("vault-20");
    ASSERT_TRUE(reg.isOffline("vault-20"));

    const auto result = reg.registerNode(makeEntry("vault-20", "New Name", "2.2.2.2", 99));
    EXPECT_EQ(result, NodeRegistry::RegisterResult::OK);

    const auto entry = reg.queryNode("vault-20");
    ASSERT_TRUE(entry.has_value());
    if (!entry)
        return; // Clang-tidy dataflow guard

    EXPECT_EQ(entry->status, NodeRegistry::NodeStatus::ONLINE);
    EXPECT_EQ(entry->bunker_name, "New Name");
    EXPECT_EQ(entry->ip_address, "2.2.2.2");
    EXPECT_EQ(entry->capacity, 99u);
}

TEST(NodeRegistryTest, reregistrationOfOnlineNodeReturnsError)
{
    NodeRegistry reg;
    reg.registerNode(makeEntry("vault-20", "Original", "1.1.1.1", 50));

    const auto result = reg.registerNode(makeEntry("vault-20", "Duplicate", "2.2.2.2", 99));
    EXPECT_EQ(result, NodeRegistry::RegisterResult::ERR_DUPLICATE);

    const auto entry = reg.queryNode("vault-20");
    ASSERT_TRUE(entry.has_value());
    if (!entry)
        return; // Clang-tidy dataflow guard

    EXPECT_EQ(entry->bunker_name, "Original");
    EXPECT_EQ(entry->status, NodeRegistry::NodeStatus::ONLINE);
}

TEST(NodeRegistryTest, setOfflineDoesNotRemoveEntry)
{
    NodeRegistry reg;
    reg.registerNode(makeEntry("vault-20"));
    reg.setOffline("vault-20");

    const auto nodes = reg.listNodes();
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].node_id, "vault-20");
    EXPECT_EQ(nodes[0].status, NodeRegistry::NodeStatus::OFFLINE);
}

TEST(NodeRegistryTest, fullOfflineOnlineCycle)
{
    NodeRegistry reg;
    reg.registerNode(makeEntry("vault-20"));
    EXPECT_FALSE(reg.isOffline("vault-20"));

    reg.setOffline("vault-20");
    EXPECT_TRUE(reg.isOffline("vault-20"));

    // QUERY returns OFFLINE, not error
    auto entry = reg.queryNode("vault-20");
    ASSERT_TRUE(entry.has_value());
    if (!entry)
        return; // Clang-tidy dataflow guard

    EXPECT_EQ(entry->status, NodeRegistry::NodeStatus::OFFLINE);

    // Re-register → back to ONLINE
    reg.registerNode(makeEntry("vault-20"));
    EXPECT_FALSE(reg.isOffline("vault-20"));

    entry = reg.queryNode("vault-20");
    ASSERT_TRUE(entry.has_value());
    if (!entry)
        return; // Clang-tidy dataflow guard

    EXPECT_EQ(entry->status, NodeRegistry::NodeStatus::ONLINE);
}
