#include "serverConfig.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>

// Internal helpers

/**
 * Attempts to read an environment variable and parse it as unsigned long.
 * Returns std::nullopt when the variable is unset, empty, non-numeric,
 * or contains trailing non-numeric characters.
 */
static std::optional<unsigned long> readUnsignedLong(const char* name)
{
    const char* raw = std::getenv(name); // NOLINT(concurrency-mt-unsafe)
    if (raw == nullptr || raw[0] == '\0')
    {
        return std::nullopt;
    }

    const std::string str(raw);
    std::size_t pos = 0;
    unsigned long val = 0;

    try
    {
        val = std::stoul(str, &pos);
    }
    catch (...)
    {
        return std::nullopt;
    }

    // Reject trailing garbage: "80abc" must not parse as 80
    if (pos != str.size())
    {
        return std::nullopt;
    }

    return val;
}

template<typename T>
struct ParseRange
{
    T m_min;
    T m_max;
    T m_defaultVal;
};

template<typename T>
static T parseEnv(const char* name, ParseRange<T> range)
{
    const auto raw = readUnsignedLong(name);

    if (!raw.has_value())
    {
        if (std::getenv(name) != nullptr) // NOLINT(concurrency-mt-unsafe)
        {
            std::cerr << "[WARN] serverConfig: " << name << " is not a valid number — using default "
                      << range.m_defaultVal << "\n";
        }
        return range.m_defaultVal;
    }

    if (raw.value() < static_cast<unsigned long>(range.m_min) || raw.value() > static_cast<unsigned long>(range.m_max))
    {
        std::cerr << "[WARN] serverConfig: " << name << "=" << raw.value() << " out of range [" << range.m_min << ", "
                  << range.m_max << "] — using default " << range.m_defaultVal << "\n";
        return range.m_defaultVal;
    }

    return static_cast<T>(raw.value());
}

/**
 * Reads a string environment variable. Returns its value when set and
 * non-empty, or the provided default when unset or empty.
 */
static std::string readStringEnv(const char* name, const char* defaultVal)
{
    const char* raw = std::getenv(name); // NOLINT(concurrency-mt-unsafe)
    if (raw == nullptr || raw[0] == '\0')
    {
        return defaultVal;
    }
    return raw;
}

// Public factory

ServerConfig ServerConfig::fromEnv()
{
    ServerConfig cfg {};

    cfg.m_port = parseEnv<uint16_t>("EOP_PORT", {1, std::numeric_limits<uint16_t>::max(), ServerConfig::DEFAULT_PORT});
    cfg.m_idleTimeoutSecs = parseEnv<uint32_t>("EOP_IDLE_TIMEOUT", {1, 3600, ServerConfig::DEFAULT_IDLE_TIMEOUT});
    cfg.m_threadPoolSize = parseEnv<uint32_t>("EOP_THREAD_POOL_SIZE", {1, 256, ServerConfig::DEFAULT_THREAD_POOL_SIZE});
    cfg.m_maxClients = parseEnv<uint32_t>("EOP_MAX_CLIENTS", {1, 100000, ServerConfig::DEFAULT_MAX_CLIENTS});
    cfg.m_heartbeatIntervalSecs =
        parseEnv<uint32_t>("EOP_HEARTBEAT_INTERVAL", {1, 300, ServerConfig::DEFAULT_HEARTBEAT_INTERVAL});
    cfg.m_hpcTimeoutSecs = parseEnv<uint32_t>("EOP_HPC_TIMEOUT", {1, 300, ServerConfig::DEFAULT_HPC_TIMEOUT});
    cfg.m_hpcNodeId = readStringEnv("HPC_NODE_ID", ServerConfig::DEFAULT_HPC_NODE_ID);

    return cfg;
}
