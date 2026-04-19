#pragma once

#include <cstdint>

/**
 * @file serverConfig.hpp
 * @brief Server configuration loaded once at startup from environment variables.
 *
 * All values are read via ServerConfig::fromEnv(). Invalid or out-of-range
 * values are logged to stderr at WARN level and replaced with documented
 * defaults. No other translation unit may contain hardcoded port or timeout
 * literals.
 */

/**
 * @brief Plain value struct holding all server runtime configuration.
 *
 * Constructed exclusively via ServerConfig::fromEnv(). Intended to be passed
 * by const reference throughout the server — never mutated after startup.
 */
struct ServerConfig
{
    // Defaults
    static constexpr uint16_t DEFAULT_PORT = 9026;
    /// HTTP /metrics (Prometheus). 0 disables exposition (tests).
    static constexpr uint16_t DEFAULT_METRICS_PORT = 9464;
    static constexpr uint32_t DEFAULT_IDLE_TIMEOUT = 30;
    static constexpr uint32_t DEFAULT_THREAD_POOL_SIZE = 16;
    static constexpr uint32_t DEFAULT_MAX_CLIENTS = 10000;
    static constexpr uint32_t DEFAULT_HEARTBEAT_INTERVAL = 5;

    // Fields
    uint16_t m_port;                  ///< EOP_PORT             (1–65535,  default 9026)
    uint16_t m_metricsPort;           ///< EOP_METRICS_PORT     (0=off, 1–65535, default 9464)
    uint32_t m_idleTimeoutSecs;       ///< EOP_IDLE_TIMEOUT     (1–3600,   default 30)
    uint32_t m_threadPoolSize;        ///< EOP_THREAD_POOL_SIZE (1–256,    default 16)
    uint32_t m_maxClients;            ///< EOP_MAX_CLIENTS      (1–100000, default 10000)
    uint32_t m_heartbeatIntervalSecs; ///< EOP_HEARTBEAT_INTERVAL (1–300,  default 5)

    /**
     * @brief Reads all environment variables and returns a validated config.
     *
     * Each variable goes through a dedicated parse helper that applies range
     * validation and falls back to the documented default on any error,
     * printing a WARN line to stderr before doing so.
     *
     * @return Fully populated ServerConfig.
     */
    static ServerConfig fromEnv();
};
