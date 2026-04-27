#include "sessionManager.hpp"

#include <iomanip>
#include <sstream>

// Width of the hex-encoded session ID string
static constexpr int SESSION_ID_HEX_WIDTH = 16;

std::string SessionManager::nextSessionId()
{
    const uint64_t id = m_sessionCounter.fetch_add(1, std::memory_order_relaxed) + 1;

    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(SESSION_ID_HEX_WIDTH) << id;
    return oss.str();
}

uint32_t SessionManager::nextMessageId()
{
    return m_messageCounter.fetch_add(1, std::memory_order_relaxed) + 1;
}
