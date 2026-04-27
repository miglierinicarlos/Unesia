/**
 * @file eop_demo.c
 * @brief Interactive console demo for the EOP client library.
 * @details Connects to an EOP server and lets the user exercise the full
 * v0.1 + v0.2 protocol: REGISTER, QUERY_NODE, LIST_NODES, HEARTBEAT, and
 * ANALYZE_GRAPH.
 *
 * Usage:
 * ./eop_demo [host] [port] [timeout_ms]
 * ./eop_demo 127.0.0.1 9026 5000
 */

#include "eop_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── constants ─────────────────────────────────────────────────────────────── */

#define DEFAULT_HOST       "127.0.0.1"
#define DEFAULT_PORT       9026
#define CONNECT_TIMEOUT_MS 5000
#define ERR_MSG_SIZE       256
#define JSON_BUF_SIZE      512
#define INPUT_BUF_SIZE     128
#define TRACE_ID_BUF_SIZE  33 /* 32 hex chars + NUL */

/* ANALYZE_GRAPH demo limits */
#define MAX_DEMO_NODES        32
#define MAX_DEMO_EDGES_TOTAL  128
#define ANALYZE_JSON_BUF_SIZE 8192 /* sufficient for MAX_DEMO_NODES * MAX_DEMO_EDGES_TOTAL */

/* ── internal types ─────────────────────────────────────────────────────────── */

typedef struct
{
    int destination;
    int weight;
} demo_edge_t;

typedef struct
{
    demo_edge_t edges[MAX_DEMO_EDGES_TOTAL];
    int count;
} demo_node_t;

/* ── helpers ───────────────────────────────────────────────────────────────── */

static void strip_newline(char* s)
{
    size_t len = strlen(s);
    if (len > 0 && s[len - 1] == '\n')
        s[len - 1] = '\0';
}

static void print_payload(const eop_response_t* resp)
{
    if (resp->payload == NULL || resp->payload_len == 0)
    {
        printf("  (empty payload)\n");
        return;
    }
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

static const char* msg_type_name(eop_message_type_t t)
{
    switch (t)
    {
        case EOP_ACK: return "ACK";
        case EOP_ERROR: return "ERROR";
        case EOP_ANALYZE_RESULT: return "ANALYZE_RESULT";
        default: return "UNKNOWN";
    }
}

/* Generate a 32-char hex trace ID from time + pid (demo quality only). */
static void generate_trace_id(char* out)
{
    unsigned long t = (unsigned long)time(NULL);
    unsigned long p = (unsigned long)getpid();
    snprintf(out, TRACE_ID_BUF_SIZE, "%016lx%016lx", t ^ (p << 16), p ^ (t << 16));
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

    eop_response_t* resp = eop_send_command(client, EOP_HEARTBEAT, NULL, 0);
    if (resp == NULL)
    {
        char err[ERR_MSG_SIZE];
        eop_error_code code = eop_last_error(client, err, sizeof(err));
        if (code == EOP_ERR_TIMEOUT)
            printf("  [OK] Heartbeat sent (no response from server — expected)\n");
        else
            fprintf(stderr, "  [error] %s\n", err);
        return;
    }
    printf("  [%s] ", msg_type_name(resp->msg_type));
    print_payload(resp);
    eop_response_free(resp);
}

/* Build the adjacency JSON into buf (size buf_size) from the node table.
 * Returns the number of bytes written, or -1 on overflow. */
static int
build_adjacency_json(char* buf, size_t buf_size, const demo_node_t* nodes, int num_nodes, const char* trace_id)
{
    int pos = 0;
    int rem = (int)buf_size;
    int w;

    w = snprintf(buf + pos, (size_t)rem, "{\"adjacency\":[");
    if (w < 0 || w >= rem)
        return -1;
    pos += w;
    rem -= w;

    for (int n = 0; n < num_nodes; ++n)
    {
        w = snprintf(buf + pos, (size_t)rem, "[");
        if (w < 0 || w >= rem)
            return -1;
        pos += w;
        rem -= w;

        for (int e = 0; e < nodes[n].count; ++e)
        {
            w = snprintf(buf + pos,
                         (size_t)rem,
                         "%s{\"destination\":%d,\"weight\":%d}",
                         (e > 0) ? "," : "",
                         nodes[n].edges[e].destination,
                         nodes[n].edges[e].weight);
            if (w < 0 || w >= rem)
                return -1;
            pos += w;
            rem -= w;
        }

        w = snprintf(buf + pos, (size_t)rem, "%s", (n < num_nodes - 1) ? "]," : "]");
        if (w < 0 || w >= rem)
            return -1;
        pos += w;
        rem -= w;
    }

    if (trace_id[0] != '\0')
    {
        w = snprintf(buf + pos, (size_t)rem, "],\"trace_id\":\"%s\"}", trace_id);
    }
    else
    {
        w = snprintf(buf + pos, (size_t)rem, "]}");
    }
    if (w < 0 || w >= rem)
        return -1;
    pos += w;

    return pos;
}

static void cmd_analyze_graph(eop_client_t* client)
{
    char input[INPUT_BUF_SIZE];
    char trace_id[TRACE_ID_BUF_SIZE];
    demo_node_t nodes[MAX_DEMO_NODES];
    int num_nodes = 0;
    int mode;

    printf("\n--- ANALYZE GRAPH ---\n");
    printf("  Graph input mode:\n");
    printf("    1) Line preset  — directed chain 0→1→2→…→N-1\n");
    printf("    2) Custom       — enter edges manually\n");
    printf("  Mode: ");
    fflush(stdout);
    fgets(input, sizeof(input), stdin);
    mode = atoi(input);

    if (mode != 1 && mode != 2)
    {
        fprintf(stderr, "  [error] Invalid mode\n");
        return;
    }

    printf("  Number of nodes (1-%d): ", MAX_DEMO_NODES);
    fflush(stdout);
    fgets(input, sizeof(input), stdin);
    strip_newline(input);
    num_nodes = atoi(input);

    if (num_nodes < 1 || num_nodes > MAX_DEMO_NODES)
    {
        fprintf(stderr, "  [error] Nodes must be in [1, %d]\n", MAX_DEMO_NODES);
        return;
    }

    /* Zero-initialise adjacency table */
    for (int i = 0; i < num_nodes; ++i) nodes[i].count = 0;

    if (mode == 1)
    {
        /* Directed line graph: each node i → i+1 with weight 1 */
        for (int i = 0; i < num_nodes - 1; ++i)
        {
            nodes[i].edges[0].destination = i + 1;
            nodes[i].edges[0].weight = 1;
            nodes[i].count = 1;
        }
        printf("  Built line graph: 0→1→…→%d\n", num_nodes - 1);
    }
    else
    {
        int total_edges = 0;
        int num_edges;

        printf("  Number of edges (0-%d): ", MAX_DEMO_EDGES_TOTAL);
        fflush(stdout);
        fgets(input, sizeof(input), stdin);
        strip_newline(input);
        num_edges = atoi(input);

        if (num_edges < 0 || num_edges > MAX_DEMO_EDGES_TOTAL)
        {
            fprintf(stderr, "  [error] Edges must be in [0, %d]\n", MAX_DEMO_EDGES_TOTAL);
            return;
        }

        for (int e = 0; e < num_edges; ++e)
        {
            char src_str[INPUT_BUF_SIZE];
            char dst_str[INPUT_BUF_SIZE];
            char w_str[INPUT_BUF_SIZE];
            int src, dst, w;

            printf("  Edge %d — src (0-%d): ", e + 1, num_nodes - 1);
            fflush(stdout);
            fgets(src_str, sizeof(src_str), stdin);
            strip_newline(src_str);
            src = atoi(src_str);

            printf("  Edge %d — dst (0-%d): ", e + 1, num_nodes - 1);
            fflush(stdout);
            fgets(dst_str, sizeof(dst_str), stdin);
            strip_newline(dst_str);
            dst = atoi(dst_str);

            printf("  Edge %d — weight    : ", e + 1);
            fflush(stdout);
            fgets(w_str, sizeof(w_str), stdin);
            strip_newline(w_str);
            w = atoi(w_str);

            if (src < 0 || src >= num_nodes || dst < 0 || dst >= num_nodes)
            {
                fprintf(stderr, "  [warn] Edge %d skipped — node index out of range\n", e + 1);
                continue;
            }
            if (w <= 0)
            {
                fprintf(stderr, "  [warn] Edge %d skipped — weight must be > 0\n", e + 1);
                continue;
            }
            if (nodes[src].count >= MAX_DEMO_EDGES_TOTAL)
            {
                fprintf(stderr, "  [warn] Edge %d skipped — node %d edge list full\n", e + 1, src);
                continue;
            }

            nodes[src].edges[nodes[src].count].destination = dst;
            nodes[src].edges[nodes[src].count].weight = w;
            nodes[src].count++;
            ++total_edges;
        }

        printf("  Added %d edge(s) across %d node(s).\n", total_edges, num_nodes);
    }

    /* Optional trace_id for OTel correlation in SigNoz */
    printf("  trace_id [Enter to auto-generate]: ");
    fflush(stdout);
    fgets(input, sizeof(input), stdin);
    strip_newline(input);
    if (input[0] == '\0')
        generate_trace_id(trace_id);
    else
        snprintf(trace_id, sizeof(trace_id), "%.32s", input);

    printf("  trace_id: %s\n", trace_id);

    /* Serialise payload */
    char* json_buf = malloc(ANALYZE_JSON_BUF_SIZE);
    if (json_buf == NULL)
    {
        fprintf(stderr, "  [error] Memory allocation failed\n");
        return;
    }

    int json_len = build_adjacency_json(json_buf, ANALYZE_JSON_BUF_SIZE, nodes, num_nodes, trace_id);
    if (json_len < 0)
    {
        fprintf(stderr, "  [error] Graph JSON too large for buffer\n");
        free(json_buf);
        return;
    }

    printf("  Sending ANALYZE_GRAPH (%d bytes)...\n", json_len);
    fflush(stdout);

    eop_response_t* resp = eop_send_command(client, EOP_ANALYZE_GRAPH, (const uint8_t*)json_buf, (size_t)json_len);
    free(json_buf);

    if (resp == NULL)
    {
        char err[ERR_MSG_SIZE];
        eop_error_code code = eop_last_error(client, err, sizeof(err));
        if (code == EOP_ERR_TIMEOUT)
            fprintf(stderr, "  [error] Timed out waiting for result — is the HPC engine running?\n");
        else
            fprintf(stderr, "  [error] %s\n", err);
        return;
    }

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
    printf("║  5) Analyze graph (HPC)      ║\n");
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

    /* Drain the welcome ACK the server sends on every new connection. */
    eop_response_t* welcome = eop_send_command(client, EOP_HEARTBEAT, NULL, 0);
    if (welcome != NULL)
        eop_response_free(welcome);

    char line[INPUT_BUF_SIZE];
    int running = 1;

    while (running)
    {
        print_menu();
        if (fgets(line, sizeof(line), stdin) == NULL)
            break;
        strip_newline(line);

        switch (atoi(line))
        {
            case 1: cmd_register(client); break;
            case 2: cmd_query_node(client); break;
            case 3: cmd_list_nodes(client); break;
            case 4: cmd_heartbeat(client); break;
            case 5: cmd_analyze_graph(client); break;
            case 0: running = 0; break;
            default: printf("  Unknown option.\n"); break;
        }
    }

    eop_disconnect(client);
    printf("Disconnected.\n");
    return EXIT_SUCCESS;
}
