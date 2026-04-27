/**
 * @file eop_client.h
 * @brief Public API for the EOP client library.
 * @details Provides an opaque handle to manage TCP connections to an EOP
 *          server. The caller creates a connection with eop_connect() and
 *          inspects errors with eop_last_error(). All resources are hidden
 *          behind the opaque eop_client_t handle.
 */

#ifndef EOP_CLIENT_H
#define EOP_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#define EOP_FRAME_PREFIX_SIZE 4

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Message types used in communication with the EOP server.
     */
    typedef enum eop_message_type_t
    {
        EOP_REGISTER = 0x01,
        EOP_ACK = 0x02,
        EOP_ERROR = 0x03,
        EOP_QUERY_NODE = 0x04,
        EOP_LIST_NODES = 0x05,
        EOP_HEARTBEAT = 0x06,
        EOP_ANALYZE_GRAPH = 0x07,
        EOP_ANALYZE_RESULT = 0x08,
        EOP_EDGE_TELEMETRY = 0x09,
    } eop_message_type_t;

    /**
     * @brief Error codes returned by the client library.
     */
    typedef enum eop_error_code
    {
        EOP_OK = 0,                       /**< No error. */
        EOP_ERR_TIMEOUT = 1,              /**< Connection or operation timed out. */
        EOP_ERR_DISCONNECTED = 2,         /**< Server closed the connection. */
        EOP_ERR_CONNECT_FAILED = 3,       /**< Unable to establish a TCP connection. */
        EOP_ERR_INVALID_ARGUMENT = 4,     /**< Invalid argument passed to a function. */
        EOP_ERR_ALLOC = 5,                /**< Memory allocation failed. */
        EOP_ERR_NOT_ENOUGH_SPACE = 6,     /**< Output buffer is too small. */
        EOP_ERR_INVALID_MESSAGE_TYPE = 7, /**< Invalid message type specified. */
    } eop_error_code;

    /**
     * @brief Opaque handle representing a client connection.
     */
    typedef struct eop_client_t eop_client_t;

    /**
     * @brief Response received from the EOP server.
     * @details Allocated by eop_send_command() on success. The caller is
     *          responsible for freeing it with eop_response_free().
     */
    typedef struct eop_response_t
    {
        eop_message_type_t msg_type; /**< Message type of the response (e.g. EOP_ACK, EOP_ERROR). */
        uint32_t msg_id;             /**< Message ID echoed from the request. */
        uint8_t* payload;            /**< Heap-allocated payload buffer. NULL if payload_len is 0. */
        size_t payload_len;          /**< Length of @p payload in bytes. */
    } eop_response_t;

    /**
     * @brief Establish a TCP connection to an EOP server.
     *
     * @param host        Null-terminated hostname or IPv4 address. Must not be NULL.
     * @param port        Port number in the range [1, 65535].
     * @param timeout_ms  Connection timeout in milliseconds. Values <= 0 use the
     *                    internal default (5000 ms). The function never blocks
     *                    indefinitely.
     *
     * @return Pointer to a new eop_client_t on success, or NULL on failure.
     *         On failure, call eop_last_error() to retrieve the reason.
     *
     * @pre  @p host is a valid null-terminated string, @p port is in [1, 65535], and
     *       @p timeout_ms is either > 0 (caller-specified) or <= 0 (use default).
     * @post On success the returned handle owns an open TCP socket.
     */
    eop_client_t* eop_connect(const char* host, int port, int timeout_ms);

    /**
     * @brief Retrieve the last error code and an optional human-readable message.
     *
     * @param client      Handle returned by eop_connect(), or NULL to query
     *                    the last global error (e.g. when eop_connect() failed).
     * @param msg         Buffer to receive the error message, or NULL to ignore.
     * @param buffer_size Size of @p msg in bytes.
     *
     * @return The eop_error_code that describes the last error.
     *
     * @pre  If @p msg is not NULL, @p buffer_size must be greater than 0.
     * @post If @p msg is not NULL, it contains a null-terminated string.
     */
    eop_error_code eop_last_error(eop_client_t* client, char* msg, size_t buffer_size);

    /**
     * @brief Close the connection and release all resources associated with the client handle.
     *
     * @details Performs a three-step teardown: (1) calls @c shutdown(SHUT_RDWR) on the socket so
     *          the server receives a FIN segment, (2) closes the file descriptor, (3) destroys the
     *          internal mutex and frees the handle. Calling this function with @p client set to
     *          @c NULL is a safe no-op — no crash, no undefined behavior.
     *
     * @param client Handle returned by eop_connect(), or @c NULL.
     *
     * @pre  @p client is either @c NULL or a valid handle returned by eop_connect() that has not
     *       been disconnected yet.
     * @post The TCP connection is closed, all resources are freed, and @p client is invalidated.
     *       The caller must not dereference or pass @p client to any function after this call.
     *
     * @warning Using the handle after @c eop_disconnect() is undefined behavior. Always
     *          set the pointer to @c NULL immediately after calling this function:
     * @code
     *   eop_disconnect(client);
     *   client = NULL;
     * @endcode
     */
    void eop_disconnect(eop_client_t* client);

    /**
     * @brief Free a response returned by eop_send_command().
     *
     * @param response Pointer to the response to free, or NULL (safe no-op).
     *
     * @pre  @p response was returned by eop_send_command() or is NULL.
     * @post All memory owned by @p response is freed. The pointer must not
     *       be used after this call.
     */
    void eop_response_free(eop_response_t* response);

    /**
     * @brief Reconnect a client handle to the EOP server after a disconnection.
     *
     * @details Closes the current (broken) socket, creates a new TCP socket, and
     *          reconnects to the same host and port used in eop_connect(). The
     *          handle's internal state (host, port, timeout) is preserved. All
     *          resources owned by the handle remain valid after a successful call.
     *          On failure the handle remains in a disconnected state and must be
     *          either reconnected again or released with eop_disconnect().
     *
     * @param client     Handle returned by eop_connect(). Must not be NULL.
     * @param timeout_ms Reconnect timeout in milliseconds. Values <= 0 reuse the
     *                   timeout supplied to the original eop_connect() call.
     *
     * @return EOP_OK on success, or an eop_error_code describing the failure.
     *         On failure, call eop_last_error() to retrieve the reason.
     *
     * @pre  @p client is a valid (possibly disconnected) handle returned by
     *       eop_connect() that has not been freed with eop_disconnect().
     * @post On EOP_OK the handle owns a new open TCP socket ready for use.
     *       On failure the old socket is closed; the handle is in a disconnected
     *       state and must be reconnected or freed with eop_disconnect().
     *
     * @warning Do not call this function while another thread is blocked inside
     *          eop_send_command() on the same handle — the socket replacement is
     *          not safe under concurrent I/O.
     */
    eop_error_code eop_reconnect(eop_client_t* client, int timeout_ms);

    /**
     * @brief Returns the underlying socket file descriptor.
     *
     * @details Intended for use cases where raw frame I/O is required on the
     *          same connection managed by the client handle (e.g. the HPC engine
     *          main processing loop). The caller must not close or otherwise
     *          modify the descriptor; ownership remains with the handle.
     *
     * @param client Handle returned by eop_connect(), or NULL.
     *
     * @return The socket file descriptor, or -1 if @p client is NULL.
     *
     * @pre  @p client is either NULL or a valid connected handle.
     * @post The descriptor is valid as long as @p client has not been passed
     *       to eop_disconnect().
     */
    int eop_get_fd(const eop_client_t* client);

    /**
     * @brief Serialize and send a command to the EOP server, then block until a complete response is received.
     *
     * @param client      Handle returned by eop_connect(). Must not be NULL.
     * @param msg_type    Type of the command message to send.
     * @param payload     Pointer to the payload bytes, or NULL if @p payload_len is 0.
     * @param payload_len Length of @p payload in bytes.
     *
     * @return Pointer to a heap-allocated eop_response_t on success, or NULL on failure.
     *         On failure, call eop_last_error() to retrieve the reason.
     *         The caller is responsible for freeing the returned pointer with eop_response_free().
     *
     * @pre  @p client is a valid connected handle. If @p payload_len > 0, @p payload must not be NULL.
     * @post On success, the returned response owns a heap-allocated payload buffer.
     *
     * @note Thread-safe. Multiple threads may call this function concurrently on
     *       the same @p client handle. Socket I/O is serialized via an internal
     *       mutex; each call blocks until a complete response is received.
     */
    eop_response_t*
    eop_send_command(eop_client_t* client, eop_message_type_t msg_type, const uint8_t* payload, size_t payload_len);

#ifdef __cplusplus
}
#endif

#endif /* EOP_CLIENT_H */
