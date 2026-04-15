#include "heartbeatHandler.hpp"
#include "logger.hpp"
#include "nodeRegistry.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

/* Helper: builds a NodeEntry with all fields populated */
static NodeRegistry::NodeEntry makeEntry(const std::string& nodeId,
                                         const std::string& bunker = "Bunker-A",
                                         const std::string& ip = "10.0.0.1",
                                         uint64_t cap = 100)
{
    return {nodeId, bunker, ip, cap};
}

// =============================================================================
// HeartbeatHandlerTest.heartbeatUpdatesLastSeenAt
// =============================================================================
TEST(HeartbeatHandlerTest, heartbeatUpdatesLastSeenAt)
{
    NodeRegistry reg;
    reg.registerNode(makeEntry("vault-01"));
    Logger logger(Logger::Level::NONE);
    HeartbeatHandler handler(reg, logger);

    const auto before = std::chrono::system_clock::now();
    handler.handle("vault-01", "sess-01");
    const auto after = std::chrono::system_clock::now();

    const auto entry = reg.queryNode("vault-01");
    ASSERT_TRUE(entry.has_value());
    if (!entry)
        return; // Clang-tidy dataflow guard

    EXPECT_GE(entry->last_seen_at, before);
    EXPECT_LE(entry->last_seen_at, after);
}

// =============================================================================
// HeartbeatHandlerTest.heartbeatSendsNoResponse
// =============================================================================
// Verified structurally: handle() has no SendFn parameter by design (ADR-003).
// This test confirms the handler compiles and runs without any send mechanism,
// and that it does not modify registry state in unexpected ways.
TEST(HeartbeatHandlerTest, heartbeatSendsNoResponse)
{
    NodeRegistry reg;
    reg.registerNode(makeEntry("vault-01"));
    Logger logger(Logger::Level::NONE);
    HeartbeatHandler handler(reg, logger);

    // No sender param — calling handle() is the entire proof of the contract
    EXPECT_NO_FATAL_FAILURE(handler.handle("vault-01", "sess-01"));

    // Registry state: still ONLINE, nothing broken
    const auto entry = reg.queryNode("vault-01");
    ASSERT_TRUE(entry.has_value());
    if (!entry)
        return; // Clang-tidy dataflow guard

    EXPECT_EQ(entry->status, NodeRegistry::NodeStatus::ONLINE);
}

// =============================================================================
// HeartbeatHandlerTest.heartbeatForUnknownNodeIsNoOp
// =============================================================================
TEST(HeartbeatHandlerTest, heartbeatForUnknownNodeIsNoOp)
{
    NodeRegistry reg;
    Logger logger(Logger::Level::NONE);
    HeartbeatHandler handler(reg, logger);

    // Must not crash; node must not appear in registry afterwards
    EXPECT_NO_FATAL_FAILURE(handler.handle("ghost-node", "sess-x"));
    EXPECT_FALSE(reg.contains("ghost-node"));
}

// =============================================================================
// HeartbeatHandlerTest.heartbeatForOfflineNodeIsNoOp
// =============================================================================
TEST(HeartbeatHandlerTest, heartbeatForOfflineNodeIsNoOp)
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

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    Logger logger(Logger::Level::NONE);
    HeartbeatHandler handler(reg, logger);
    handler.handle("vault-01", "sess-01");

    // last_seen_at must not change — OFFLINE node heartbeat is ignored
    const auto entry = reg.queryNode("vault-01");
    ASSERT_TRUE(entry.has_value());
    if (!entry)
        return; // Clang-tidy dataflow guard

    EXPECT_EQ(entry->last_seen_at, lastSeenAfterOffline);
}
