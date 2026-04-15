#include "nodeRegistry.hpp"

#include <mutex>
#include <shared_mutex>

NodeRegistry::RegisterResult NodeRegistry::registerNode(NodeEntry entry)
{
    std::unique_lock lock(m_mutex);

    if (entry.node_id.empty())
    {
        return RegisterResult::ERR_DUPLICATE;
    }

    const auto it = m_entries.find(entry.node_id);
    if (it != m_entries.end())
    {
        if (it->second.status == NodeStatus::ONLINE)
        {
            return RegisterResult::ERR_DUPLICATE;
        }
        /* OFFLINE node re-registering: update all metadata, set ONLINE */
        entry.status = NodeStatus::ONLINE;
        it->second = std::move(entry);
        return RegisterResult::OK;
    }

    entry.status = NodeStatus::ONLINE;
    const std::string key = entry.node_id;
    m_entries.emplace(key, std::move(entry));
    return RegisterResult::OK;
}

std::optional<NodeRegistry::NodeEntry> NodeRegistry::queryNode(const std::string& nodeId) const
{
    std::shared_lock lock(m_mutex);
    const auto it = m_entries.find(nodeId);
    if (it == m_entries.end())
    {
        return std::nullopt;
    }
    return it->second;
}

std::vector<NodeRegistry::NodeEntry> NodeRegistry::listNodes() const
{
    std::shared_lock lock(m_mutex);
    std::vector<NodeEntry> snapshot;
    snapshot.reserve(m_entries.size());
    for (const auto& [id, entry] : m_entries)
    {
        snapshot.push_back(entry);
    }
    return snapshot;
}

void NodeRegistry::setOffline(const std::string& nodeId)
{
    if (nodeId.empty())
    {
        return;
    }
    std::unique_lock lock(m_mutex);
    const auto it = m_entries.find(nodeId);
    if (it != m_entries.end())
    {
        it->second.status = NodeStatus::OFFLINE;
        it->second.last_seen_at = std::chrono::system_clock::now();
    }
}

bool NodeRegistry::updateLastSeen(const std::string& nodeId)
{
    if (nodeId.empty())
    {
        return false;
    }
    std::unique_lock lock(m_mutex);
    const auto it = m_entries.find(nodeId);
    if (it != m_entries.end() && it->second.status == NodeStatus::ONLINE)
    {
        it->second.last_seen_at = std::chrono::system_clock::now();
        return true;
    }
    return false;
}

bool NodeRegistry::isOffline(const std::string& nodeId) const
{
    std::shared_lock lock(m_mutex);
    const auto it = m_entries.find(nodeId);
    if (it == m_entries.end())
    {
        return false;
    }
    return it->second.status == NodeStatus::OFFLINE;
}

bool NodeRegistry::contains(const std::string& nodeId) const
{
    std::shared_lock lock(m_mutex);
    return m_entries.contains(nodeId);
}
