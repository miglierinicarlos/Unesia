#include "serverConfig.hpp"

#include <cstdlib>
#include <gtest/gtest.h>
#include <string>

// Helpers

static void setEnv(const char* name, const char* value)
{
    ::setenv(name, value, /*overwrite=*/1); // NOLINT(concurrency-mt-unsafe) -- called once before any thread is spawned
}

static void unsetEnv(const char* name)
{
    ::unsetenv(name); // NOLINT(concurrency-mt-unsafe) -- called once before any thread is spawned
}

/**
 * RAII guard: saves the current value of an env var on construction,
 * unsets it for the duration of the test, and restores it on destruction.
 * Ensures tests are independent regardless of execution order.
 */
struct EnvGuard
{
    std::string m_name;
    std::string m_previous;
    bool m_wasSet = false;

    explicit EnvGuard(const char* varName)
        : m_name(varName)
    {
        const char* val = std::getenv(varName); // NOLINT(concurrency-mt-unsafe) -- tests run single-threaded
        m_wasSet = (val != nullptr);
        m_previous = m_wasSet ? val : "";
        unsetEnv(varName);
    }

    ~EnvGuard()
    {
        if (m_wasSet)
            setEnv(m_name.c_str(), m_previous.c_str());
        else
            unsetEnv(m_name.c_str());
    }

    // Non-copyable
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;
};

// EOP_PORT

TEST(serverConfigTest, defaultPortWhenUnset)
{
    EnvGuard g("EOP_PORT");
    const ServerConfig cfg = ServerConfig::fromEnv();
    EXPECT_EQ(cfg.m_port, ServerConfig::DEFAULT_PORT);
}

TEST(serverConfigTest, validPortIsUsed)
{
    EnvGuard g("EOP_PORT");
    setEnv("EOP_PORT", "8080");
    const ServerConfig cfg = ServerConfig::fromEnv();
    EXPECT_EQ(cfg.m_port, 8080);
}

TEST(serverConfigTest, invalidPortNonNumericFallsBackToDefault)
{
    EnvGuard g("EOP_PORT");
    setEnv("EOP_PORT", "abc");
    const ServerConfig cfg = ServerConfig::fromEnv();
    EXPECT_EQ(cfg.m_port, ServerConfig::DEFAULT_PORT);
}

TEST(serverConfigTest, invalidPortTrailingCharsFallsBackToDefault)
{
    EnvGuard g("EOP_PORT");
    setEnv("EOP_PORT", "80abc");
    const ServerConfig cfg = ServerConfig::fromEnv();
    EXPECT_EQ(cfg.m_port, ServerConfig::DEFAULT_PORT);
}

TEST(serverConfigTest, portZeroFallsBackToDefault)
{
    EnvGuard g("EOP_PORT");
    setEnv("EOP_PORT", "0");
    const ServerConfig cfg = ServerConfig::fromEnv();
    EXPECT_EQ(cfg.m_port, ServerConfig::DEFAULT_PORT);
}

TEST(serverConfigTest, portMaxValueIsAccepted)
{
    EnvGuard g("EOP_PORT");
    setEnv("EOP_PORT", "65535");
    const ServerConfig cfg = ServerConfig::fromEnv();
    EXPECT_EQ(cfg.m_port, 65535);
}

// EOP_IDLE_TIMEOUT

TEST(serverConfigTest, defaultIdleTimeoutWhenUnset)
{
    EnvGuard g("EOP_IDLE_TIMEOUT");
    const ServerConfig cfg = ServerConfig::fromEnv();
    EXPECT_EQ(cfg.m_idleTimeoutSecs, ServerConfig::DEFAULT_IDLE_TIMEOUT);
}

TEST(serverConfigTest, validIdleTimeoutIsUsed)
{
    EnvGuard g("EOP_IDLE_TIMEOUT");
    setEnv("EOP_IDLE_TIMEOUT", "60");
    const ServerConfig cfg = ServerConfig::fromEnv();
    EXPECT_EQ(cfg.m_idleTimeoutSecs, 60u);
}

TEST(serverConfigTest, idleTimeoutZeroFallsBackToDefault)
{
    EnvGuard g("EOP_IDLE_TIMEOUT");
    setEnv("EOP_IDLE_TIMEOUT", "0");
    const ServerConfig cfg = ServerConfig::fromEnv();
    EXPECT_EQ(cfg.m_idleTimeoutSecs, ServerConfig::DEFAULT_IDLE_TIMEOUT);
}

TEST(serverConfigTest, idleTimeoutAboveMaxFallsBackToDefault)
{
    EnvGuard g("EOP_IDLE_TIMEOUT");
    setEnv("EOP_IDLE_TIMEOUT", "3601");
    const ServerConfig cfg = ServerConfig::fromEnv();
    EXPECT_EQ(cfg.m_idleTimeoutSecs, ServerConfig::DEFAULT_IDLE_TIMEOUT);
}

// EOP_THREAD_POOL_SIZE

TEST(serverConfigTest, defaultThreadPoolSizeWhenUnset)
{
    EnvGuard g("EOP_THREAD_POOL_SIZE");
    const ServerConfig cfg = ServerConfig::fromEnv();
    EXPECT_EQ(cfg.m_threadPoolSize, ServerConfig::DEFAULT_THREAD_POOL_SIZE);
}

TEST(serverConfigTest, validThreadPoolSizeIsUsed)
{
    EnvGuard g("EOP_THREAD_POOL_SIZE");
    setEnv("EOP_THREAD_POOL_SIZE", "32");
    const ServerConfig cfg = ServerConfig::fromEnv();
    EXPECT_EQ(cfg.m_threadPoolSize, 32u);
}

TEST(serverConfigTest, threadPoolSizeAboveMaxFallsBackToDefault)
{
    EnvGuard g("EOP_THREAD_POOL_SIZE");
    setEnv("EOP_THREAD_POOL_SIZE", "257");
    const ServerConfig cfg = ServerConfig::fromEnv();
    EXPECT_EQ(cfg.m_threadPoolSize, ServerConfig::DEFAULT_THREAD_POOL_SIZE);
}

TEST(serverConfigTest, threadPoolSizeZeroFallsBackToDefault)
{
    EnvGuard g("EOP_THREAD_POOL_SIZE");
    setEnv("EOP_THREAD_POOL_SIZE", "0");
    const ServerConfig cfg = ServerConfig::fromEnv();
    EXPECT_EQ(cfg.m_threadPoolSize, ServerConfig::DEFAULT_THREAD_POOL_SIZE);
}

// EOP_MAX_CLIENTS

TEST(serverConfigTest, defaultMaxClientsWhenUnset)
{
    EnvGuard g("EOP_MAX_CLIENTS");
    const ServerConfig cfg = ServerConfig::fromEnv();
    EXPECT_EQ(cfg.m_maxClients, ServerConfig::DEFAULT_MAX_CLIENTS);
}

TEST(serverConfigTest, validMaxClientsIsUsed)
{
    EnvGuard g("EOP_MAX_CLIENTS");
    setEnv("EOP_MAX_CLIENTS", "500");
    const ServerConfig cfg = ServerConfig::fromEnv();
    EXPECT_EQ(cfg.m_maxClients, 500u);
}

TEST(serverConfigTest, maxClientsAboveMaxFallsBackToDefault)
{
    EnvGuard g("EOP_MAX_CLIENTS");
    setEnv("EOP_MAX_CLIENTS", "100001");
    const ServerConfig cfg = ServerConfig::fromEnv();
    EXPECT_EQ(cfg.m_maxClients, ServerConfig::DEFAULT_MAX_CLIENTS);
}

TEST(serverConfigTest, maxClientsZeroFallsBackToDefault)
{
    EnvGuard g("EOP_MAX_CLIENTS");
    setEnv("EOP_MAX_CLIENTS", "0");
    const ServerConfig cfg = ServerConfig::fromEnv();
    EXPECT_EQ(cfg.m_maxClients, ServerConfig::DEFAULT_MAX_CLIENTS);
}

// EOP_HEARTBEAT_INTERVAL

TEST(serverConfigTest, defaultHeartbeatIntervalWhenUnset)
{
    EnvGuard g("EOP_HEARTBEAT_INTERVAL");
    const ServerConfig cfg = ServerConfig::fromEnv();
    EXPECT_EQ(cfg.m_heartbeatIntervalSecs, ServerConfig::DEFAULT_HEARTBEAT_INTERVAL);
}

TEST(serverConfigTest, validHeartbeatIntervalIsUsed)
{
    EnvGuard g("EOP_HEARTBEAT_INTERVAL");
    setEnv("EOP_HEARTBEAT_INTERVAL", "10");
    const ServerConfig cfg = ServerConfig::fromEnv();
    EXPECT_EQ(cfg.m_heartbeatIntervalSecs, 10u);
}

TEST(serverConfigTest, heartbeatIntervalAboveMaxFallsBackToDefault)
{
    EnvGuard g("EOP_HEARTBEAT_INTERVAL");
    setEnv("EOP_HEARTBEAT_INTERVAL", "301");
    const ServerConfig cfg = ServerConfig::fromEnv();
    EXPECT_EQ(cfg.m_heartbeatIntervalSecs, ServerConfig::DEFAULT_HEARTBEAT_INTERVAL);
}

TEST(serverConfigTest, heartbeatIntervalZeroFallsBackToDefault)
{
    EnvGuard g("EOP_HEARTBEAT_INTERVAL");
    setEnv("EOP_HEARTBEAT_INTERVAL", "0");
    const ServerConfig cfg = ServerConfig::fromEnv();
    EXPECT_EQ(cfg.m_heartbeatIntervalSecs, ServerConfig::DEFAULT_HEARTBEAT_INTERVAL);
}
