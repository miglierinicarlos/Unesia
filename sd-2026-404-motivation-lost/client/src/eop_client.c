#include "eop_client.h"
#include "eop_envelope.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define EOP_CONNECT_TIMEOUT    5000
#define EOP_COMMAND_TIMEOUT_MS 5000
#define EOP_ERROR_MSG_SIZE     256
#define EOP_FRAME_PREFIX_SIZE  4
#define EOP_MAX_HOST_LEN       256
#define TCP_PORT_MAX_VALUE     65535
#define TCP_PORT_MIN_VALUE     1

static __thread eop_error_code s_last_error = EOP_OK;
static __thread char s_last_error_msg[EOP_ERROR_MSG_SIZE] = {0};

static void set_global_error(eop_error_code code, const char* msg)
{
    s_last_error = code;
    strncpy(s_last_error_msg, msg, EOP_ERROR_MSG_SIZE - 1);
    s_last_error_msg[EOP_ERROR_MSG_SIZE - 1] = '\0';
}

struct eop_client_t
{
    int socket_fd;
    int last_error;
    char last_error_msg[EOP_ERROR_MSG_SIZE];
    int connection_timeout_ms;
    uint32_t next_msg_id;
    pthread_mutex_t mutex;
    char host[EOP_MAX_HOST_LEN];
    int port;
};

static void set_client_error(eop_client_t* client, eop_error_code code, const char* msg)
{
    client->last_error = (int)code;
    strncpy(client->last_error_msg, msg, EOP_ERROR_MSG_SIZE - 1);
    client->last_error_msg[EOP_ERROR_MSG_SIZE - 1] = '\0';
}

eop_client_t* eop_connect(const char* host, int port, int timeout_ms)
{
    if (host == NULL)
    {
        set_global_error(EOP_ERR_INVALID_ARGUMENT, "Invalid host");
        return NULL;
    }
    if (port < TCP_PORT_MIN_VALUE || port > TCP_PORT_MAX_VALUE)
    {
        set_global_error(EOP_ERR_INVALID_ARGUMENT, "Invalid port number");
        return NULL;
    }

    eop_client_t* client = malloc(sizeof(eop_client_t));
    if (client == NULL)
    {
        set_global_error(EOP_ERR_ALLOC, "Failed to allocate client handle");
        return NULL;
    }
    client->connection_timeout_ms = (timeout_ms > 0) ? timeout_ms : EOP_CONNECT_TIMEOUT;
    client->last_error = EOP_OK;
    client->last_error_msg[0] = '\0';
    client->next_msg_id = 0;
    strncpy(client->host, host, EOP_MAX_HOST_LEN - 1);
    client->host[EOP_MAX_HOST_LEN - 1] = '\0';
    client->port = port;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL)
    {
        set_global_error(EOP_ERR_CONNECT_FAILED, "Failed to resolve host");
        free(client);
        return NULL;
    }

    client->socket_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

    if (client->socket_fd < 0)
    {
        freeaddrinfo(res);
        set_global_error(EOP_ERR_CONNECT_FAILED, "Failed to create socket");
        free(client);
        return NULL;
    }

    fcntl(client->socket_fd, F_SETFL, O_NONBLOCK);
    int connect_result = connect(client->socket_fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (connect_result < 0 && errno != EINPROGRESS)
    {
        set_global_error(EOP_ERR_CONNECT_FAILED, "Connection refused");
        close(client->socket_fd);
        free(client);
        return NULL;
    }

    int poll_result = poll((struct pollfd[]) {{client->socket_fd, POLLOUT, 0}}, 1, client->connection_timeout_ms);
    if (poll_result == 0)
    {
        set_global_error(EOP_ERR_TIMEOUT, "Connection timed out");
        close(client->socket_fd);
        free(client);
        return NULL;
    }
    if (poll_result < 0)
    {
        set_global_error(EOP_ERR_CONNECT_FAILED, "Poll failed during connection");
        close(client->socket_fd);
        free(client);
        return NULL;
    }

    int so_error;
    socklen_t len = sizeof(so_error);
    if (getsockopt(client->socket_fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0)
    {
        set_global_error(EOP_ERR_CONNECT_FAILED, "Connection failed after poll");
        close(client->socket_fd);
        free(client);
        return NULL;
    }

    fcntl(client->socket_fd, F_SETFL, fcntl(client->socket_fd, F_GETFL, 0) & ~O_NONBLOCK);

    pthread_mutex_init(&client->mutex, NULL);
    set_global_error(EOP_OK, "");

    return client;
}

eop_error_code eop_last_error(eop_client_t* client, char* msg, size_t buffer_size)
{
    if (client == NULL)
    {
        if (msg != NULL && buffer_size > 0)
        {
            strncpy(msg, s_last_error_msg, buffer_size - 1);
            msg[buffer_size - 1] = '\0';
        }
        return s_last_error;
    }

    pthread_mutex_lock(&client->mutex);
    eop_error_code code = client->last_error;
    if (msg != NULL && buffer_size > 0)
    {
        strncpy(msg, client->last_error_msg, buffer_size - 1);
        msg[buffer_size - 1] = '\0';
    }
    pthread_mutex_unlock(&client->mutex);
    return code;
}

void eop_disconnect(eop_client_t* client)
{
    if (client == NULL)
    {
        return;
    }
    shutdown(client->socket_fd, SHUT_RDWR);
    close(client->socket_fd);
    pthread_mutex_destroy(&client->mutex);
    free(client);
}

eop_error_code eop_reconnect(eop_client_t* client, int timeout_ms)
{
    if (client == NULL)
    {
        set_global_error(EOP_ERR_INVALID_ARGUMENT, "Invalid client handle");
        return EOP_ERR_INVALID_ARGUMENT;
    }

    pthread_mutex_lock(&client->mutex);

    /* Close the broken socket — ignore errors; the connection may already be dead */
    shutdown(client->socket_fd, SHUT_RDWR);
    close(client->socket_fd);
    client->socket_fd = -1;

    int effective_timeout = (timeout_ms > 0) ? timeout_ms : client->connection_timeout_ms;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", client->port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = NULL;
    if (getaddrinfo(client->host, port_str, &hints, &res) != 0 || res == NULL)
    {
        set_client_error(client, EOP_ERR_CONNECT_FAILED, "Failed to resolve host on reconnect");
        pthread_mutex_unlock(&client->mutex);
        return EOP_ERR_CONNECT_FAILED;
    }

    int new_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (new_fd < 0)
    {
        freeaddrinfo(res);
        set_client_error(client, EOP_ERR_CONNECT_FAILED, "Failed to create socket on reconnect");
        pthread_mutex_unlock(&client->mutex);
        return EOP_ERR_CONNECT_FAILED;
    }

    fcntl(new_fd, F_SETFL, O_NONBLOCK);
    int connect_result = connect(new_fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (connect_result < 0 && errno != EINPROGRESS)
    {
        set_client_error(client, EOP_ERR_CONNECT_FAILED, "Reconnect refused");
        close(new_fd);
        pthread_mutex_unlock(&client->mutex);
        return EOP_ERR_CONNECT_FAILED;
    }

    int poll_result = poll((struct pollfd[]) {{new_fd, POLLOUT, 0}}, 1, effective_timeout);
    if (poll_result == 0)
    {
        set_client_error(client, EOP_ERR_TIMEOUT, "Reconnect timed out");
        close(new_fd);
        pthread_mutex_unlock(&client->mutex);
        return EOP_ERR_TIMEOUT;
    }
    if (poll_result < 0)
    {
        set_client_error(client, EOP_ERR_CONNECT_FAILED, "Poll failed during reconnect");
        close(new_fd);
        pthread_mutex_unlock(&client->mutex);
        return EOP_ERR_CONNECT_FAILED;
    }

    int so_error;
    socklen_t len = sizeof(so_error);
    if (getsockopt(new_fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0)
    {
        set_client_error(client, EOP_ERR_CONNECT_FAILED, "Reconnect failed after poll");
        close(new_fd);
        pthread_mutex_unlock(&client->mutex);
        return EOP_ERR_CONNECT_FAILED;
    }

    fcntl(new_fd, F_SETFL, fcntl(new_fd, F_GETFL, 0) & ~O_NONBLOCK);

    client->socket_fd = new_fd;
    client->last_error = (int)EOP_OK;
    client->last_error_msg[0] = '\0';

    pthread_mutex_unlock(&client->mutex);
    return EOP_OK;
}

int eop_get_fd(const eop_client_t* client)
{
    return (client != NULL) ? client->socket_fd : -1;
}

void eop_response_free(eop_response_t* response)
{
    if (response == NULL)
    {
        return;
    }
    free(response->payload);
    free(response);
}

static eop_response_t*
eop_response_create(eop_message_type_t msg_type, uint32_t msg_id, uint8_t* payload, size_t payload_len)
{
    eop_response_t* response = malloc(sizeof(eop_response_t));
    if (response == NULL)
    {
        return NULL;
    }
    response->msg_type = msg_type;
    response->msg_id = msg_id;
    response->payload = payload;
    response->payload_len = payload_len;
    return response;
}

eop_response_t*
eop_send_command(eop_client_t* client, eop_message_type_t msg_type, const uint8_t* payload, size_t payload_len)
{
    if (client == NULL)
    {
        set_global_error(EOP_ERR_INVALID_ARGUMENT, "Invalid client handle");
        return NULL;
    }
    if (payload == NULL && payload_len > 0)
    {
        set_global_error(EOP_ERR_INVALID_ARGUMENT, "Payload is NULL but payload_len > 0");
        return NULL;
    }

    pthread_mutex_lock(&client->mutex);

    /* Assign and increment message ID atomically under the lock */
    uint32_t msg_id = client->next_msg_id++;

    /* Allocate buffer: 4-byte frame_length prefix + 10-byte envelope header + payload */
    size_t frame_body_size = EOP_ENVELOPE_HEADER_SIZE + payload_len;
    size_t send_buf_size = EOP_FRAME_PREFIX_SIZE + frame_body_size;
    uint8_t* send_buf = malloc(send_buf_size);
    if (send_buf == NULL)
    {
        set_client_error(client, EOP_ERR_ALLOC, "Failed to allocate send buffer");
        pthread_mutex_unlock(&client->mutex);
        return NULL;
    }

    /* Write frame_length prefix (ADR-003: total bytes that follow, BE) */
    uint32_t frame_length_net = htonl((uint32_t)frame_body_size);
    memcpy(send_buf, &frame_length_net, EOP_FRAME_PREFIX_SIZE);

    int rc = eop_envelope_serialize(
        msg_type, msg_id, payload, payload_len, send_buf + EOP_FRAME_PREFIX_SIZE, frame_body_size);
    if (rc != (int)EOP_OK)
    {
        set_client_error(client, (eop_error_code)rc, "Failed to serialize envelope");
        free(send_buf);
        pthread_mutex_unlock(&client->mutex);
        return NULL;
    }

    /* Send loop — handle partial writes */
    size_t total_sent = 0;
    while (total_sent < send_buf_size)
    {
        ssize_t sent = send(client->socket_fd, send_buf + total_sent, send_buf_size - total_sent, 0);
        if (sent < 0)
        {
            set_client_error(client, EOP_ERR_DISCONNECTED, "Send failed");
            free(send_buf);
            pthread_mutex_unlock(&client->mutex);
            return NULL;
        }
        total_sent += (size_t)sent;
    }
    free(send_buf);

    /* Phase 0 — receive 4-byte frame_length prefix (ADR-003) */
    uint8_t frame_prefix_buf[EOP_FRAME_PREFIX_SIZE];
    size_t total_recv = 0;
    while (total_recv < EOP_FRAME_PREFIX_SIZE)
    {
        struct pollfd pfd = {client->socket_fd, POLLIN, 0};
        int pr = poll(&pfd, 1, EOP_COMMAND_TIMEOUT_MS);
        if (pr == 0)
        {
            set_client_error(client, EOP_ERR_TIMEOUT, "Timeout waiting for frame length prefix");
            pthread_mutex_unlock(&client->mutex);
            return NULL;
        }
        if (pr < 0)
        {
            set_client_error(client, EOP_ERR_DISCONNECTED, "Poll error waiting for frame length prefix");
            pthread_mutex_unlock(&client->mutex);
            return NULL;
        }
        ssize_t r = recv(client->socket_fd, frame_prefix_buf + total_recv, EOP_FRAME_PREFIX_SIZE - total_recv, 0);
        if (r == 0)
        {
            set_client_error(client, EOP_ERR_DISCONNECTED, "Server closed connection");
            pthread_mutex_unlock(&client->mutex);
            return NULL;
        }
        if (r < 0)
        {
            set_client_error(client, EOP_ERR_DISCONNECTED, "Recv error reading frame length prefix");
            pthread_mutex_unlock(&client->mutex);
            return NULL;
        }
        total_recv += (size_t)r;
    }

    /* Phase 1 — receive exactly EOP_ENVELOPE_HEADER_SIZE bytes */
    uint8_t header_buf[EOP_ENVELOPE_HEADER_SIZE];
    total_recv = 0;
    while (total_recv < EOP_ENVELOPE_HEADER_SIZE)
    {
        struct pollfd pfd = {client->socket_fd, POLLIN, 0};
        int pr = poll(&pfd, 1, EOP_COMMAND_TIMEOUT_MS);
        if (pr == 0)
        {
            set_client_error(client, EOP_ERR_TIMEOUT, "Timeout waiting for response header");
            pthread_mutex_unlock(&client->mutex);
            return NULL;
        }
        if (pr < 0)
        {
            set_client_error(client, EOP_ERR_DISCONNECTED, "Poll error waiting for response header");
            pthread_mutex_unlock(&client->mutex);
            return NULL;
        }
        ssize_t r = recv(client->socket_fd, header_buf + total_recv, EOP_ENVELOPE_HEADER_SIZE - total_recv, 0);
        if (r == 0)
        {
            set_client_error(client, EOP_ERR_DISCONNECTED, "Server closed connection");
            pthread_mutex_unlock(&client->mutex);
            return NULL;
        }
        if (r < 0)
        {
            set_client_error(client, EOP_ERR_DISCONNECTED, "Recv error reading response header");
            pthread_mutex_unlock(&client->mutex);
            return NULL;
        }
        total_recv += (size_t)r;
    }

    /* Extract fields directly from the raw header bytes */
    if (header_buf[0] != EOP_PROTOCOL_VERSION)
    {
        set_client_error(client, EOP_ERR_DISCONNECTED, "Unsupported protocol version in response");
        pthread_mutex_unlock(&client->mutex);
        return NULL;
    }
    eop_message_type_t resp_msg_type = (eop_message_type_t)header_buf[1];
    uint32_t resp_msg_id_net;
    memcpy(&resp_msg_id_net, header_buf + 2, sizeof(resp_msg_id_net));
    uint32_t resp_msg_id = ntohl(resp_msg_id_net);
    uint32_t resp_payload_len_net;
    memcpy(&resp_payload_len_net, header_buf + 6, sizeof(resp_payload_len_net));
    size_t resp_payload_len = (size_t)ntohl(resp_payload_len_net);

    /* Phase 2 — receive exactly resp_payload_len bytes */
    uint8_t* resp_payload = NULL;
    if (resp_payload_len > 0)
    {
        resp_payload = malloc(resp_payload_len);
        if (resp_payload == NULL)
        {
            set_client_error(client, EOP_ERR_ALLOC, "Failed to allocate response payload buffer");
            pthread_mutex_unlock(&client->mutex);
            return NULL;
        }

        total_recv = 0;
        while (total_recv < resp_payload_len)
        {
            struct pollfd pfd = {client->socket_fd, POLLIN, 0};
            int pr = poll(&pfd, 1, EOP_COMMAND_TIMEOUT_MS);
            if (pr == 0)
            {
                set_client_error(client, EOP_ERR_TIMEOUT, "Timeout waiting for response payload");
                free(resp_payload);
                pthread_mutex_unlock(&client->mutex);
                return NULL;
            }
            if (pr < 0)
            {
                set_client_error(client, EOP_ERR_DISCONNECTED, "Poll error waiting for response payload");
                free(resp_payload);
                pthread_mutex_unlock(&client->mutex);
                return NULL;
            }
            ssize_t r = recv(client->socket_fd, resp_payload + total_recv, resp_payload_len - total_recv, 0);
            if (r == 0)
            {
                set_client_error(client, EOP_ERR_DISCONNECTED, "Server closed connection during payload recv");
                free(resp_payload);
                pthread_mutex_unlock(&client->mutex);
                return NULL;
            }
            if (r < 0)
            {
                set_client_error(client, EOP_ERR_DISCONNECTED, "Recv error reading response payload");
                free(resp_payload);
                pthread_mutex_unlock(&client->mutex);
                return NULL;
            }
            total_recv += (size_t)r;
        }
    }

    /* Allocate and fill response struct */
    eop_response_t* response = eop_response_create(resp_msg_type, resp_msg_id, resp_payload, resp_payload_len);
    if (response == NULL)
    {
        set_client_error(client, EOP_ERR_ALLOC, "Failed to allocate response struct");
        free(resp_payload);
        pthread_mutex_unlock(&client->mutex);
        return NULL;
    }

    client->last_error = (int)EOP_OK;
    client->last_error_msg[0] = '\0';

    pthread_mutex_unlock(&client->mutex);
    return response;
}
