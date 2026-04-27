/**
 * @file eop_client_syscall_fail_test.cpp
 * @brief Tests for eop_connect error paths triggered by syscall failures.
 *
 * Uses GNU ld --wrap to intercept malloc, socket, connect and poll.
 * Each wrapper delegates to the real implementation unless a
 * thread-local flag requests a forced failure.
 */

extern "C"
{
#include "eop_client.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>

    /* Real implementations provided by the linker. */
    void* __real_malloc(size_t size);
    int __real_socket(int domain, int type, int protocol);
    int __real_connect(int fd, const struct sockaddr* addr, socklen_t addrlen);
    int __real_poll(struct pollfd* fds, nfds_t nfds, int timeout);

    /* Flags that control when the wrapped function should fail. */
    static thread_local bool g_fail_malloc = false;
    static thread_local bool g_fail_socket = false;
    static thread_local bool g_fail_connect = false;
    static thread_local bool g_fail_poll = false;

    void* __wrap_malloc(size_t size)
    {
        if (g_fail_malloc)
        {
            g_fail_malloc = false;
            return NULL;
        }
        return __real_malloc(size);
    }

    int __wrap_socket(int domain, int type, int protocol)
    {
        if (g_fail_socket)
        {
            g_fail_socket = false;
            errno = EMFILE;
            return -1;
        }
        return __real_socket(domain, type, protocol);
    }

    int __wrap_connect(int fd, const struct sockaddr* addr, socklen_t addrlen)
    {
        if (g_fail_connect)
        {
            g_fail_connect = false;
            errno = EACCES;
            return -1;
        }
        return __real_connect(fd, addr, addrlen);
    }

    int __wrap_poll(struct pollfd* fds, nfds_t nfds, int timeout)
    {
        if (g_fail_poll)
        {
            g_fail_poll = false;
            errno = EINTR;
            return -1;
        }
        return __real_poll(fds, nfds, timeout);
    }

} /* extern "C" */

#include <gtest/gtest.h>

#include <cstring>

static const int ERR_MSG_SIZE = 256;

TEST(SyscallFailTest, MallocFailReturnsNull)
{
    g_fail_malloc = true;
    eop_client_t* client = eop_connect("127.0.0.1", 9999, 0);
    ASSERT_EQ(client, nullptr);

    char msg[ERR_MSG_SIZE];
    eop_error_code err = eop_last_error(nullptr, msg, sizeof(msg));
    EXPECT_EQ(err, EOP_ERR_ALLOC);
}

TEST(SyscallFailTest, SocketFailReturnsNull)
{
    g_fail_socket = true;
    eop_client_t* client = eop_connect("127.0.0.1", 9999, 0);
    ASSERT_EQ(client, nullptr);

    char msg[ERR_MSG_SIZE];
    eop_error_code err = eop_last_error(nullptr, msg, sizeof(msg));
    EXPECT_EQ(err, EOP_ERR_CONNECT_FAILED);
}

TEST(SyscallFailTest, ConnectImmediateFailReturnsNull)
{
    g_fail_connect = true;
    eop_client_t* client = eop_connect("127.0.0.1", 9999, 0);
    ASSERT_EQ(client, nullptr);

    char msg[ERR_MSG_SIZE];
    eop_error_code err = eop_last_error(nullptr, msg, sizeof(msg));
    EXPECT_EQ(err, EOP_ERR_CONNECT_FAILED);
}

TEST(SyscallFailTest, PollFailReturnsNull)
{
    g_fail_poll = true;
    eop_client_t* client = eop_connect("127.0.0.1", 9999, 0);
    ASSERT_EQ(client, nullptr);

    char msg[ERR_MSG_SIZE];
    eop_error_code err = eop_last_error(nullptr, msg, sizeof(msg));
    EXPECT_EQ(err, EOP_ERR_CONNECT_FAILED);
}
