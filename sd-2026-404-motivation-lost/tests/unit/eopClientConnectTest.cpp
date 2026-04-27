extern "C"
{
#include "eop_client.h"
}

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

static const int LISTEN_BACKLOG = 1024;
static const int STRESS_CYCLES = 1000;
static const char* BLACKHOLE_HOST = "10.255.255.1"; /* Non-routable, triggers timeout */
static const int BLACKHOLE_PORT = 9999;
static const char* REFUSED_HOST = "127.0.0.1"; /* Localhost, no listener = refused */
static const int REFUSED_PORT = 1;             /* Privileged port, nothing listens */
static const int ERR_MSG_SIZE = 256;

class ConnectTest : public ::testing::Test
{
protected:
    int m_listenFd = -1;
    int m_port = 0;
    std::atomic<bool> m_acceptRunning {false};
    std::thread m_acceptThread;

    void SetUp() override
    {
        m_listenFd = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(m_listenFd, 0) << "Failed to create listener socket";

        int opt = 1;
        setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(0); /* OS assigns a free port */

        ASSERT_EQ(bind(m_listenFd, (struct sockaddr*)&addr, sizeof(addr)), 0) << "Failed to bind listener";

        /* Retrieve the assigned port */
        socklen_t len = sizeof(addr);
        ASSERT_EQ(getsockname(m_listenFd, (struct sockaddr*)&addr, &len), 0);
        m_port = ntohs(addr.sin_port);

        ASSERT_EQ(listen(m_listenFd, LISTEN_BACKLOG), 0) << "Failed to listen";

        /* Accept and close connections in a background thread.
           The listen socket is set to non-blocking so the accept loop
           can check m_acceptRunning and exit cleanly. */
        fcntl(m_listenFd, F_SETFL, O_NONBLOCK);
        m_acceptRunning = true;
        m_acceptThread = std::thread(
            [this]()
            {
                while (m_acceptRunning)
                {
                    struct sockaddr_in clientAddr;
                    socklen_t clientLen = sizeof(clientAddr);
                    int clientFd = accept(m_listenFd, (struct sockaddr*)&clientAddr, &clientLen);
                    if (clientFd >= 0)
                    {
                        close(clientFd);
                    }
                    else
                    {
                        usleep(1000); /* 1 ms — avoid busy loop */
                    }
                }
            });
    }

    void TearDown() override
    {
        m_acceptRunning = false;
        if (m_listenFd >= 0)
        {
            close(m_listenFd);
        }
        if (m_acceptThread.joinable())
        {
            m_acceptThread.join();
        }
    }
};

TEST_F(ConnectTest, SuccessReturnsValidHandle)
{
    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);
    eop_disconnect(client);
}

TEST_F(ConnectTest, UnreachableHostReturnsNull)
{
    eop_client_t* client = eop_connect(REFUSED_HOST, REFUSED_PORT, 0);
    ASSERT_EQ(client, nullptr);

    char msg[ERR_MSG_SIZE];
    eop_error_code err = eop_last_error(nullptr, msg, sizeof(msg));
    EXPECT_EQ(err, EOP_ERR_CONNECT_FAILED);
    EXPECT_GT(std::strlen(msg), 0u);
}

TEST_F(ConnectTest, TimeoutEnforced)
{
    auto start = std::chrono::steady_clock::now();
    eop_client_t* client = eop_connect(BLACKHOLE_HOST, BLACKHOLE_PORT, 5000);
    auto end = std::chrono::steady_clock::now();

    ASSERT_EQ(client, nullptr);

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    /* Should not block much longer than the 5000 ms timeout (1 s tolerance) */
    EXPECT_LE(elapsed.count(), 6000);
    /* Should block for at least most of the timeout */
    EXPECT_GE(elapsed.count(), 4000);
}

TEST_F(ConnectTest, NullHandleLastError)
{
    /* Calling eop_last_error with NULL handle must not crash */
    char msg[ERR_MSG_SIZE];
    eop_error_code err = eop_last_error(nullptr, msg, sizeof(msg));
    (void)err;

    /* Also safe with NULL buffer */
    err = eop_last_error(nullptr, nullptr, 0);
    (void)err;
}

TEST_F(ConnectTest, NullHostReturnsNull)
{
    eop_client_t* client = eop_connect(nullptr, m_port, 0);
    ASSERT_EQ(client, nullptr);

    char msg[ERR_MSG_SIZE];
    eop_error_code err = eop_last_error(nullptr, msg, sizeof(msg));
    EXPECT_EQ(err, EOP_ERR_INVALID_ARGUMENT);
    EXPECT_GT(std::strlen(msg), 0u);
}

TEST_F(ConnectTest, InvalidPortReturnsNull)
{
    eop_client_t* client = eop_connect("127.0.0.1", 0, 0);
    ASSERT_EQ(client, nullptr);

    char msg[ERR_MSG_SIZE];
    eop_error_code err = eop_last_error(nullptr, msg, sizeof(msg));
    EXPECT_EQ(err, EOP_ERR_INVALID_ARGUMENT);
    EXPECT_GT(std::strlen(msg), 0u);
}

TEST_F(ConnectTest, InvalidHostReturnsNull)
{
    eop_client_t* client = eop_connect("not-an-ip", m_port, 0);
    ASSERT_EQ(client, nullptr);

    char msg[ERR_MSG_SIZE];
    eop_error_code err = eop_last_error(nullptr, msg, sizeof(msg));
    EXPECT_EQ(err, EOP_ERR_CONNECT_FAILED);
    EXPECT_GT(std::strlen(msg), 0u);
}

TEST_F(ConnectTest, LastErrorWithValidClient)
{
    eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
    ASSERT_NE(client, nullptr);

    char msg[ERR_MSG_SIZE];
    eop_error_code err = eop_last_error(client, msg, sizeof(msg));
    EXPECT_EQ(err, EOP_OK);

    /* Also test with NULL buffer */
    err = eop_last_error(client, nullptr, 0);
    EXPECT_EQ(err, EOP_OK);

    eop_disconnect(client);
}

TEST_F(ConnectTest, DisconnectNullIsSafe)
{
    eop_disconnect(nullptr);
}

TEST_F(ConnectTest, StressCycle1000_ASan)
{
    for (int i = 0; i < STRESS_CYCLES; ++i)
    {
        eop_client_t* client = eop_connect("127.0.0.1", m_port, 0);
        ASSERT_NE(client, nullptr) << "Failed on cycle " << i;
        eop_disconnect(client);
    }
}
