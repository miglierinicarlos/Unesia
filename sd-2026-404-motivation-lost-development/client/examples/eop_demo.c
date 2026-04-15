/**
 * @file eop_demo.c
 * @brief Interactive console demo for the EOP client library.
 * @details Connects to an EOP server and lets the user exercise the full
 * v0.1 protocol: REGISTER, QUERY_NODE, LIST_NODES, and HEARTBEAT.
 *
 * Usage:
 * ./eop_demo [host] [port] [timeout_ms]
 * ./eop_demo 172.20.0.2 9026 5000
 */

#include "eop_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── constants ─────────────────────────────────────────────────────────────── */

#define DEFAULT_HOST       "127.0.0.1"
#define DEFAULT_PORT       9026
#define CONNECT_TIMEOUT_MS 5000
#define ERR_MSG_SIZE       256
#define JSON_BUF_SIZE      512
#define INPUT_BUF_SIZE     128

/* ── helpers ───────────────────────────────────────────────────────────────── */

/** Strip the trailing newline left by fgets(). */
static void strip_newline(char* s)
{
    size_t len = strlen(s);
    if (len > 0 && s[len - 1] == '\n')
    {
        s[len - 1] = '\0';
    }
}

/** Print a response payload as a readable string (null-terminated copy). */
static void print_payload(const eop_response_t* resp)
{
    if (resp->payload == NULL || resp->payload_len == 0)
    {
        printf("  (empty payload)\n");
        return;
    }
    /* Temporarily null-terminate for printing */
    char* buf = malloc(resp->payload_len + 1);
    if (buf == NULL)
    {
        printf("  (alloc error while printing payload)\n");
        return;
    }
    memcpy(buf, resp->payload, resp->payload_len);
    buf[resp->payload_len] = '\0';
    printf("  %s\n", buf);
    free(buf);
}

/** Describe a response message type. */
static const char* msg_type_name(eop_message_type_t t)
{
    switch (t)
    {
        case EOP_ACK: return "ACK";
        case EOP_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

/* ── command handlers ──────────────────────────────────────────────────────── */

static void cmd_register(eop_client_t* client)
{
    char node_id[INPUT_BUF_SIZE];
    char bunker_name[INPUT_BUF_SIZE];
    char ip_address[INPUT_BUF_SIZE];
    char capacity_str[INPUT_BUF_SIZE];

    printf("\n--- REGISTER NODE ---\n");
    printf("  node_id      : ");
    fflush(stdout);
    fgets(node_id, sizeof(node_id), stdin);
    strip_newline(node_id);
    printf("  bunker_name  : ");
    fflush(stdout);
    fgets(bunker_name, sizeof(bunker_name), stdin);
    strip_newline(bunker_name);
    printf("  ip_address   : ");
    fflush(stdout);
    fgets(ip_address, sizeof(ip_address), stdin);
    strip_newline(ip_address);
    printf("  capacity     : ");
    fflush(stdout);
    fgets(capacity_str, sizeof(capacity_str), stdin);
    strip_newline(capacity_str);

    unsigned long capacity = strtoul(capacity_str, NULL, 10);

    char json[JSON_BUF_SIZE];
    int written = snprintf(json,
                           sizeof(json),
                           "{\"node_id\":\"%s\",\"bunker_name\":\"%s\",\"ip_address\":\"%s\",\"capacity\":%lu}",
                           node_id,
                           bunker_name,
                           ip_address,
                           capacity);

    if (written < 0 || (size_t)written >= sizeof(json))
    {
        fprintf(stderr, "  [error] JSON payload too large\n");
        return;
    }

    eop_response_t* resp = eop_send_command(client, EOP_REGISTER, (const uint8_t*)json, (size_t)written);

    if (resp == NULL)
    {
        char err[ERR_MSG_SIZE];
        eop_last_error(client, err, sizeof(err));
        fprintf(stderr, "  [error] %s\n", err);
        return;
    }

    printf("  [%s] ", msg_type_name(resp->msg_type));
    print_payload(resp);
    eop_response_free(resp);
}

static void cmd_query_node(eop_client_t* client)
{
    char node_id[INPUT_BUF_SIZE];

    printf("\n--- QUERY NODE ---\n");
    printf("  node_id: ");
    fflush(stdout);
    fgets(node_id, sizeof(node_id), stdin);
    strip_newline(node_id);

    char json[JSON_BUF_SIZE];
    int written = snprintf(json, sizeof(json), "{\"node_id\":\"%s\"}", node_id);

    if (written < 0 || (size_t)written >= sizeof(json))
    {
        fprintf(stderr, "  [error] JSON payload too large\n");
        return;
    }

    eop_response_t* resp = eop_send_command(client, EOP_QUERY_NODE, (const uint8_t*)json, (size_t)written);

    if (resp == NULL)
    {
        char err[ERR_MSG_SIZE];
        eop_last_error(client, err, sizeof(err));
        fprintf(stderr, "  [error] %s\n", err);
        return;
    }

    printf("  [%s] ", msg_type_name(resp->msg_type));
    print_payload(resp);
    eop_response_free(resp);
}

static void cmd_list_nodes(eop_client_t* client)
{
    printf("\n--- LIST NODES ---\n");

    eop_response_t* resp = eop_send_command(client, EOP_LIST_NODES, NULL, 0);

    if (resp == NULL)
    {
        char err[ERR_MSG_SIZE];
        eop_last_error(client, err, sizeof(err));
        fprintf(stderr, "  [error] %s\n", err);
        return;
    }

    printf("  [%s] ", msg_type_name(resp->msg_type));
    print_payload(resp);
    eop_response_free(resp);
}

static void cmd_heartbeat(eop_client_t* client)
{
    printf("\n--- HEARTBEAT ---\n");

    /*
     * The server does not send a response to HEARTBEAT (ADR-003 v0.1).
     * eop_send_command() will block until its internal timeout expires.
     * A TIMEOUT result here means the frame was delivered — the server
     * simply does not acknowledge it.
     */
    eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, NULL, 0);

    if (resp == NULL)
    {
        char err[ERR_MSG_SIZE];
        eop_error_code code = eop_last_error(client, err, sizeof(err));
        if (code == EOP_ERR_TIMEOUT)
        {
            printf("  [OK] Heartbeat sent (no response from server — expected)\n");
        }
        else
        {
            fprintf(stderr, "  [error] %s\n", err);
        }
        return;
    }

    /* Unlikely: server sent something back */
    printf("  [%s] ", msg_type_name(resp->msg_type));
    print_payload(resp);
    eop_response_free(resp);
}

/* ── main ──────────────────────────────────────────────────────────────────── */

static void print_menu(void)
{
    printf("\n╔══════════════════════════════╗\n");
    printf("║     EOP Interactive Demo     ║\n");
    printf("╠══════════════════════════════╣\n");
    printf("║  1) Register node            ║\n");
    printf("║  2) Query node               ║\n");
    printf("║  3) List nodes               ║\n");
    printf("║  4) Send heartbeat           ║\n");
    printf("║  0) Disconnect & exit        ║\n");
    printf("╚══════════════════════════════╝\n");
    printf("Choice: ");
    fflush(stdout);
}

int main(int argc, char* argv[])
{
    char host_buf[INPUT_BUF_SIZE];
    char port_buf[INPUT_BUF_SIZE];
    char timeout_buf[INPUT_BUF_SIZE];
    const char* host;
    int port;
    int timeout_ms;

    if (argc > 1)
    {
        host = argv[1];
        port = (argc > 2) ? atoi(argv[2]) : DEFAULT_PORT;
        timeout_ms = (argc > 3) ? atoi(argv[3]) : CONNECT_TIMEOUT_MS;
    }
    else
    {
        printf("Host [%s]: ", DEFAULT_HOST);
        fflush(stdout);
        if (fgets(host_buf, sizeof(host_buf), stdin) == NULL)
            return EXIT_FAILURE;
        strip_newline(host_buf);
        host = (host_buf[0] != '\0') ? host_buf : DEFAULT_HOST;

        printf("Port [%d]: ", DEFAULT_PORT);
        fflush(stdout);
        if (fgets(port_buf, sizeof(port_buf), stdin) == NULL)
            return EXIT_FAILURE;
        strip_newline(port_buf);
        port = (port_buf[0] != '\0') ? atoi(port_buf) : DEFAULT_PORT;

        printf("Timeout ms [%d]: ", CONNECT_TIMEOUT_MS);
        fflush(stdout);
        if (fgets(timeout_buf, sizeof(timeout_buf), stdin) == NULL)
            return EXIT_FAILURE;
        strip_newline(timeout_buf);
        timeout_ms = (timeout_buf[0] != '\0') ? atoi(timeout_buf) : CONNECT_TIMEOUT_MS;
    }

    printf("Connecting to %s:%d (timeout %d ms) ...\n", host, port, timeout_ms);
    fflush(stdout);

    eop_client_t* client = eop_connect(host, port, timeout_ms);
    if (client == NULL)
    {
        char err[ERR_MSG_SIZE];
        eop_last_error(NULL, err, sizeof(err));
        fprintf(stderr, "Connection failed: %s\n", err);
        return EXIT_FAILURE;
    }

    printf("Connected.\n");

    /* ── DRAIN WELCOME ACK ────────────────────────────────────────────────── */
    /*
     * The server writes a welcome ACK on every accepted connection before
     * reading any client data. We send a HEARTBEAT to drain this frame
     * so it doesn't corrupt our first real interactive command.
     */
    eop_response_t* welcome = eop_send_command(client, EOP_HEARTBEAT, NULL, 0);
    if (welcome != NULL)
    {
        eop_response_free(welcome);
    }
    /* ──────────────────────────────────────────────────────────────────────── */

    char line[INPUT_BUF_SIZE];
    int running = 1;

    while (running)
    {
        print_menu();

        if (fgets(line, sizeof(line), stdin) == NULL)
        {
            break;
        }
        strip_newline(line);

        switch (atoi(line))
        {
            case 1: cmd_register(client); break;
            case 2: cmd_query_node(client); break;
            case 3: cmd_list_nodes(client); break;
            case 4: cmd_heartbeat(client); break;
            case 0: running = 0; break;
            default: printf("  Unknown option.\n"); break;
        }
    }

    eop_disconnect(client);
    printf("Disconnected.\n");
    return EXIT_SUCCESS;
}
