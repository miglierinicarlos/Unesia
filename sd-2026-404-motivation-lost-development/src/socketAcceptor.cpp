#include "socketAcceptor.hpp"
#include "messageSerializer.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>

static constexpr int LISTEN_BACKLOG = SOMAXCONN;
static constexpr suseconds_t REJECT_TIMEOUT_US = 500000; // 500ms

SocketAcceptor::SocketAcceptor(const ServerConfig& config,
                               std::atomic<uint32_t>& activeConnections,
                               Logger& logger,
                               OnAcceptCallback onAccept)
    : m_config(config)
    , m_activeConnections(activeConnections)
    , m_logger(logger)
    , m_onAccept(std::move(onAccept))
{
}

SocketAcceptor::~SocketAcceptor()
{
    stop();
}

bool SocketAcceptor::start()
{
    const int rawFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (rawFd < 0)
    {
        m_logger.log(Logger::Level::ERROR, "socket() failed: " + std::system_category().message(errno));
        return false;
    }
    m_listenFd = FdGuard(rawFd);

    const int enable = 1;
    if (::setsockopt(m_listenFd.get(), SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0)
    {
        m_logger.log(Logger::Level::ERROR, "setsockopt(SO_REUSEADDR) failed: " + std::system_category().message(errno));
        return false;
    }

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(m_config.m_port);

    if (::bind(m_listenFd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        if (errno == EADDRINUSE)
        {
            m_logger.log(Logger::Level::ERROR,
                         "Port " + std::to_string(m_config.m_port) + " is already in use (EADDRINUSE)");
        }
        else
        {
            m_logger.log(Logger::Level::ERROR, "bind() failed: " + std::system_category().message(errno));
        }
        return false;
    }

    if (::listen(m_listenFd.get(), LISTEN_BACKLOG) < 0)
    {
        m_logger.log(Logger::Level::ERROR, "listen() failed: " + std::system_category().message(errno));
        return false;
    }

    m_logger.log(Logger::Level::INFO, "Listening on port " + std::to_string(m_config.m_port));

    m_stopFlag.store(false, std::memory_order_release);
    m_acceptThread = std::thread(&SocketAcceptor::acceptLoop, this);

    return true;
}

void SocketAcceptor::stop()
{
    m_stopFlag.store(true, std::memory_order_release);

    if (m_listenFd.isValid())
    {
        ::shutdown(m_listenFd.get(), SHUT_RDWR);
    }

    if (m_acceptThread.joinable())
    {
        m_acceptThread.join();
    }

    m_listenFd.reset();
}

void SocketAcceptor::acceptLoop()
{
    while (!m_stopFlag.load(std::memory_order_acquire))
    {
        sockaddr_in clientAddr {};
        socklen_t clientLen = sizeof(clientAddr);

        const int clientFd = ::accept(m_listenFd.get(), reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);

        if (clientFd < 0)
        {
            if (m_stopFlag.load(std::memory_order_acquire))
            {
                break;
            }
            if (errno == EINTR)
            {
                continue;
            }
            std::cerr << "[ERROR] socketAcceptor: accept() failed: " << std::system_category().message(errno) << "\n";
            break;
        }

        std::array<char, INET_ADDRSTRLEN> ipStr {};
        inet_ntop(AF_INET, &(clientAddr.sin_addr), ipStr.data(), INET_ADDRSTRLEN);
        const std::string clientIp(ipStr.data());

        const uint32_t currentCount = m_activeConnections.load(std::memory_order_acquire);
        if (currentCount >= m_config.m_maxClients)
        {
            // Apply 500ms send timeout to prevent slowloris-style blocking on rejection
            timeval tv {0, REJECT_TIMEOUT_US};
            ::setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            const std::string payload = MessageSerializer::buildErrorPayload(
                {0, MessageSerializer::ProtocolErrorCode::ERR_CAPACITY, "Server at maximum client capacity"});
            const auto frame = MessageSerializer::buildFrame(MessageSerializer::MessageType::ERROR_MSG, 0, payload);

            static_cast<void>(MessageSerializer::sendAll(clientFd, frame.data(), frame.size()));
            ::close(clientFd);

            m_logger.log(Logger::Level::WARN,
                         "Connection rejected (ERR_CAPACITY). IP: " + clientIp +
                             " | Count: " + std::to_string(currentCount));
            continue;
        }

        m_onAccept(clientFd, clientIp);
    }
}
