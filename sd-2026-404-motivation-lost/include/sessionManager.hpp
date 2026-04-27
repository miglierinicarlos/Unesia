#pragma once

#include <atomic>
#include <cstdint>
#include <string>

/**
 * @file sessionManager.hpp
 * @brief Thread-safe session ID generator based on a monotonically
 *        increasing atomic counter.
 *
 * Each call to nextSessionId() produces a unique hex-encoded identifier
 * without any mutex contention. The counter is global to the process
 * lifetime, so IDs are unique across all connections regardless of the
 * calling thread.
 */
class SessionManager
{
public:
    SessionManager() = default;

    // Non-copyable, non-movable — single instance per server.
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;
    SessionManager(SessionManager&&) = delete;
    SessionManager& operator=(SessionManager&&) = delete;

    /**
     * @brief Generates the next unique session ID.
     *
     * Returns a zero-padded 16-character lowercase hex string derived
     * from a monotonically increasing 64-bit counter. Thread-safe.
     *
     * @return Unique hex session ID (e.g. "0000000000000001").
     */
    [[nodiscard]] std::string nextSessionId();

    /**
     * @brief Returns the next message ID for per-connection sequencing.
     *
     * Monotonically increasing, globally unique across the process.
     *
     * @return Unique message ID.
     */
    [[nodiscard]] uint32_t nextMessageId();

private:
    std::atomic<uint64_t> m_sessionCounter {0};
    std::atomic<uint32_t> m_messageCounter {0};
};
