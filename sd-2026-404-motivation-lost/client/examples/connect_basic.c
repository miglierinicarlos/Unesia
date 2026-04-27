/**
 * @file connect_basic.c
 * @brief Complete EOP client lifecycle example.
 *
 * @details Demonstrates the full client lifecycle against a running EOP server:
 *          connect → drain welcome → register → query → disconnect.
 *
 *          The server sends a welcome ACK on every new TCP connection before
 *          reading any client message. Because eop_send_command() reads exactly
 *          one response frame per call, the first call on a fresh handle will
 *          consume that welcome frame. A HEARTBEAT (ADR-003: fire-and-forget,
 *          no server response) is used as the drain vehicle so the socket is
 *          aligned before the real commands are sent.
 *
 * Build (from repo root after cmake configure):
 *   cmake --build build --target connect_basic_example
 *
 * Run (EOP server must be listening, default port 9026):
 *   ./build/client/connect_basic_example [host] [port]
 */

#include "eop_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── defaults ────────────────────────────────────────────────────────────── */

#define DEFAULT_HOST       "127.0.0.1"
#define DEFAULT_PORT       9026
#define DEFAULT_TIMEOUT_MS 5000

/* ── demo node metadata ──────────────────────────────────────────────────── */

#define DEMO_NODE_ID     "vault-demo-01"
#define DEMO_BUNKER_NAME "Vault 101"
#define DEMO_IP_ADDRESS  "10.0.0.1"
#define DEMO_CAPACITY    250

/* ── buffer sizes ────────────────────────────────────────────────────────── */

#define REGISTER_PAYLOAD_SIZE 256
#define QUERY_PAYLOAD_SIZE    64
#define ERROR_MSG_SIZE        256
#define STATUS_BUF_SIZE       32

/* ── helpers ─────────────────────────────────────────────────────────────── */

/**
 * Copies response->payload into a freshly malloc'd, null-terminated C string.
 * Returns NULL if resp is NULL, payload is empty, or allocation fails.
 * Caller must free() the returned pointer.
 */
static char* payload_to_cstr(const eop_response_t* resp)
{
    if (resp == NULL || resp->payload_len == 0 || resp->payload == NULL)
    {
        return NULL;
    }
    char* buf = malloc(resp->payload_len + 1);
    if (buf == NULL)
    {
        return NULL;
    }
    memcpy(buf, resp->payload, resp->payload_len);
    buf[resp->payload_len] = '\0';
    return buf;
}

/**
 * Extracts the value of a string field from a flat JSON object.
 * Writes at most dst_size-1 characters into dst and null-terminates.
 * Returns 1 on success, 0 if the field is not found.
 *
 * Only handles simple string values ("key":"value") — sufficient for the
 * flat response payloads produced by the EOP server.
 */
static int extract_json_string(const char* json, const char* key, char* dst, size_t dst_size)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":\"", key);

    const char* pos = strstr(json, search);
    if (pos == NULL)
    {
        return 0;
    }
    pos += strlen(search);

    const char* end = strchr(pos, '"');
    if (end == NULL)
    {
        return 0;
    }

    size_t len = (size_t)(end - pos);
    if (len >= dst_size)
    {
        len = dst_size - 1;
    }
    memcpy(dst, pos, len);
    dst[len] = '\0';
    return 1;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char* argv[])
{
    const char* host = (argc >= 2) ? argv[1] : DEFAULT_HOST;
    int port = (argc >= 3) ? atoi(argv[2]) : DEFAULT_PORT;

    /* ── 1. Connect ──────────────────────────────────────────────────────── */

    /* Attempt to connect to the EOP server. */
    eop_client_t* client = eop_connect(host, port, DEFAULT_TIMEOUT_MS);

    if (client == NULL)
    {
        char msg[ERROR_MSG_SIZE];
        eop_last_error(NULL, msg, sizeof(msg));
        fprintf(stderr, "[connect] FAILED — %s\n", msg);
        return EXIT_FAILURE;
    }
    printf("[connect]    OK   host=%s port=%d\n", host, port);

    /* ── 2. Drain welcome ACK ────────────────────────────────────────────── */
    /*
     * The server writes a welcome ACK on every accepted connection before
     * reading any client data. Sending a HEARTBEAT (ADR-003: fire-and-forget,
     * no response from the server) lets the welcome occupy the "response"
     * slot without misaligning the subsequent REGISTER exchange.
     */
    {
        eop_response_t* welcome = eop_send_command(client, EOP_HEARTBEAT, NULL, 0);
        if (welcome == NULL)
        {
            fprintf(stderr, "[welcome] FAILED — could not drain welcome frame\n");
            eop_disconnect(client);
            return EXIT_FAILURE;
        }
        printf("[welcome]    OK   type=0x%02x\n", (unsigned)welcome->msg_type);
        eop_response_free(welcome);
    }

    /* ── 3. Register node ────────────────────────────────────────────────── */

    char reg_payload[REGISTER_PAYLOAD_SIZE];
    snprintf(reg_payload,
             sizeof(reg_payload),
             "{\"node_id\":\"%s\","
             "\"bunker_name\":\"%s\","
             "\"ip_address\":\"%s\","
             "\"capacity\":%d}",
             DEMO_NODE_ID,
             DEMO_BUNKER_NAME,
             DEMO_IP_ADDRESS,
             DEMO_CAPACITY);

    {
        eop_response_t* resp = eop_send_command(client, EOP_REGISTER, (const uint8_t*)reg_payload, strlen(reg_payload));
        if (resp == NULL || resp->msg_type != EOP_ACK)
        {
            char* body = payload_to_cstr(resp);
            fprintf(stderr,
                    "[register] FAILED — type=0x%02x payload=%s\n",
                    resp ? (unsigned)resp->msg_type : 0xFF,
                    body ? body : "(none)");
            free(body);
            eop_response_free(resp);
            eop_disconnect(client);
            return EXIT_FAILURE;
        }
        printf("[register]   OK   node_id=%s\n", DEMO_NODE_ID);
        eop_response_free(resp);
    }

    /* ── 4. Query node ───────────────────────────────────────────────────── */

    char query_payload[QUERY_PAYLOAD_SIZE];
    snprintf(query_payload, sizeof(query_payload), "{\"node_id\":\"%s\"}", DEMO_NODE_ID);

    {
        eop_response_t* resp =
            eop_send_command(client, EOP_QUERY_NODE, (const uint8_t*)query_payload, strlen(query_payload));
        if (resp == NULL || resp->msg_type != EOP_ACK)
        {
            char* body = payload_to_cstr(resp);
            fprintf(stderr,
                    "[query]    FAILED — type=0x%02x payload=%s\n",
                    resp ? (unsigned)resp->msg_type : 0xFF,
                    body ? body : "(none)");
            free(body);
            eop_response_free(resp);
            eop_disconnect(client);
            return EXIT_FAILURE;
        }

        char* body = payload_to_cstr(resp);
        char status[STATUS_BUF_SIZE];
        status[0] = '\0';
        if (body != NULL)
        {
            extract_json_string(body, "status", status, sizeof(status));
        }
        printf("[query]      OK   node_id=%s status=%s\n", DEMO_NODE_ID, status);
        free(body);
        eop_response_free(resp);
    }

    /* ── 5. Disconnect ───────────────────────────────────────────────────── */

    eop_disconnect(client);
    client = NULL;
    printf("[disconnect] OK\n");

    return EXIT_SUCCESS;
}
