#pragma once

#include <chrono>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @file nodeRegistry.hpp
 * @brief Thread-safe in-memory registry of EOP nodes.
 *
 * @details Stores a NodeEntry per registered node_id. Concurrent reads
 *          (queryNode, listNodes) use a shared lock; writes (registerNode,
 *          setOffline) use an exclusive lock, per ADR-002.
 *
 *          The registry is owned by the server and outlives any individual
 *          connection (US-103 AC4). Entries are never removed — nodes
 *          transition between ONLINE and OFFLINE states only.
 */
class NodeRegistry
{
public:
    /// Connectivity status of a registered node.
    enum class NodeStatus
    {
        ONLINE,
        OFFLINE
    };

    /// Result of a registerNode() call.
    enum class RegisterResult
    {
        OK,           ///< Node registered (new) or re-registered (was OFFLINE).
        ERR_DUPLICATE ///< node_id already exists and is ONLINE — not overwritten.
    };

    /**
     * @brief Full metadata record for a single registered node.
     */
    struct NodeEntry
    {
        std::string node_id;                                   ///< Unique node identifier.
        std::string bunker_name;                               ///< Human-readable bunker name.
        std::string ip_address;                                ///< Client IP address at registration time.
        uint64_t capacity {0};                                 ///< Bunker capacity (arbitrary units).
        NodeStatus status {NodeStatus::ONLINE};                ///< Current connectivity state.
        std::chrono::system_clock::time_point last_seen_at {}; ///< Timestamp of the last received heartbeat, or the
                                                               ///< disconnect time if the node never sent a heartbeat.
                                                               ///< Epoch if neither event has occurred yet.
    };

    NodeRegistry() = default;

    NodeRegistry(const NodeRegistry&) = delete;
    NodeRegistry& operator=(const NodeRegistry&) = delete;
    NodeRegistry(NodeRegistry&&) = delete;
    NodeRegistry& operator=(NodeRegistry&&) = delete;

    /**
     * @brief Register a new node or re-register an OFFLINE one.
     *
     * @details If node_id is new: inserts the entry with status ONLINE and
     *          returns OK. If node_id already exists and is OFFLINE: updates
     *          all metadata fields, sets status to ONLINE, and returns OK
     *          (re-registration path for US-104 AC5). If node_id already
     *          exists and is ONLINE: returns ERR_DUPLICATE without modifying
     *          the existing entry.
     *
     * @param entry NodeEntry to register. entry.node_id must not be empty.
     * @return RegisterResult::OK on success, RegisterResult::ERR_DUPLICATE if
     *         a node with the same node_id is already ONLINE.
     */
    RegisterResult registerNode(NodeEntry entry);

    /**
     * @brief Look up a node by its node_id.
     *
     * @param nodeId The node_id to search for.
     * @return A copy of the NodeEntry if found, std::nullopt otherwise.
     */
    [[nodiscard]] std::optional<NodeEntry> queryNode(const std::string& nodeId) const;

    /**
     * @brief Return a snapshot of all registered nodes.
     *
     * @details The snapshot is taken under a shared read lock. The returned
     *          vector is an independent copy — safe to iterate after the call.
     *
     * @return Vector of all NodeEntry objects currently in the registry.
     */
    [[nodiscard]] std::vector<NodeEntry> listNodes() const;

    /**
     * @brief Mark a known node as OFFLINE and record the disconnect time.
     *
     * @details Sets status to OFFLINE and updates last_seen_at to the current
     *          wall-clock time. No-op if nodeId is empty or not in the registry.
     *          The entry is never removed (AC4).
     *
     * @param nodeId The node_id to update.
     */
    void setOffline(const std::string& nodeId);

    /**
     * @brief Update the last_seen_at timestamp for a known ONLINE node.
     *
     * @details No-op if nodeId is empty, not present, or OFFLINE.
     *          Called by HeartbeatHandler on each received HEARTBEAT (0x06).
     *
     * @param nodeId The node_id to update.
     * @return true if the timestamp was updated, false if the node is unknown or OFFLINE.
     */
    [[nodiscard]] bool updateLastSeen(const std::string& nodeId);

    /**
     * @brief Returns true if the node is known and its status is OFFLINE.
     *
     * @param nodeId The node_id to check.
     * @return true if the node exists and is OFFLINE, false otherwise.
     */
    [[nodiscard]] bool isOffline(const std::string& nodeId) const;

    /**
     * @brief Returns true if a node with the given node_id is in the registry.
     *
     * @param nodeId The node_id to check.
     * @return true if the node_id is registered (regardless of status).
     */
    [[nodiscard]] bool contains(const std::string& nodeId) const;

private:
    mutable std::shared_mutex m_mutex;
    std::unordered_map<std::string, NodeEntry> m_entries;
};
