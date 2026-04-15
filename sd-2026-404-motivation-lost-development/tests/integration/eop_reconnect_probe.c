/**
 * @file eop_reconnect_probe.c
 * @brief CLI probe that exercises eop_reconnect() against a live server.
 *
 * @details Used by pod_restart_recovery_flow.sh to validate AC6/AC7 of US-109
 *          with the real C client library instead of a raw Python socket.
 *
 *          Lifecycle:
 *            1. Connect to host:port and send EOP_REGISTER.
 *            2. Print "REGISTERED\n" to stdout (shell script reads this as a
 *               synchronisation point before deleting the pod).
 *            3. Poll the server with periodic HEARTBEAT messages until the
 *               connection breaks (EOP_ERR_DISCONNECTED).
 *            4. Call eop_reconnect() in a retry loop with exponential backoff
 *               (see constants below).  The loop terminates when either the
 *               reconnect succeeds or MAX_RECONNECT_ATTEMPTS is exceeded.
 *            5. Re-send EOP_REGISTER to restore server-side node state.
 *            6. Print "RECONNECTED\n" to stdout and exit 0.
 *
 *          Exit codes:
 *            0  Full reconnect-and-reregister cycle succeeded.
 *            1  Fatal error (usage, initial connect, or reconnect exhausted).
 *
 * Usage:
 *   eop_reconnect_probe <host> <port> <node_id>
 *
 *   The probe reads no interactive input.  The shell script coordinates timing
 *   by reading lines from the probe's stdout.
 */

#include "eop_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── Retry / backoff parameters (ADR-015 recommended values) ─────────────── */
#define RECONNECT_INITIAL_DELAY_MS   500
#define RECONNECT_BACKOFF_MULTIPLIER 2
#define RECONNECT_MAX_DELAY_MS       5000
#define RECONNECT_JITTER_PCT         10
#define MAX_RECONNECT_ATTEMPTS       10
#define CONNECT_TIMEOUT_MS           3000

/* ── Heartbeat polling interval while waiting for pod crash ──────────────── */
#define HEARTBEAT_INTERVAL_MS 200
#define HEARTBEAT_WAIT_MAX_MS 60000

/* ── JSON payload template ────────────────────────────────────────────────── */
#define REGISTER_PAYLOAD_FMT                                                                                           \
    "{\"node_id\":\"%s\",\"bunker_name\":\"Reconnect Probe\",\"ip_address\":\"127.0.0.1\",\"capacity\":1}"

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int jitter_ms(int base_ms)
{
    /* ±RECONNECT_JITTER_PCT% of base_ms */
    int range = base_ms * RECONNECT_JITTER_PCT / 100;
    if (range == 0)
        return base_ms;
    return base_ms + (rand() % (2 * range + 1)) - range;
}

static int do_register(eop_client_t* client, const char* node_id)
{
    char payload[256];
    snprintf(payload, sizeof(payload), REGISTER_PAYLOAD_FMT, node_id);

    eop_response_t* resp = eop_send_command(client, EOP_REGISTER, (const uint8_t*)payload, strlen(payload));

    if (resp == NULL)
    {
        char msg[256];
        eop_last_error(client, msg, sizeof(msg));
        fprintf(stderr, "[probe] REGISTER failed: %s\n", msg);
        return -1;
    }

    if (resp->msg_type != EOP_ACK)
    {
        fprintf(stderr, "[probe] REGISTER: expected ACK, got type 0x%02X\n", resp->msg_type);
        eop_response_free(resp);
        return -1;
    }

    eop_response_free(resp);
    return 0;
}

int main(int argc, char* argv[])
{
    if (argc != 4)
    {
        fprintf(stderr, "Usage: %s <host> <port> <node_id>\n", argv[0]);
        return 1;
    }

    const char* host = argv[1];
    const int port = atoi(argv[2]);
    const char* node_id = argv[3];

    srand((unsigned int)time(NULL));

    /* ── Phase 1: initial connect + register ─────────────────────────────── */
    eop_client_t* client = eop_connect(host, port, CONNECT_TIMEOUT_MS);
    if (client == NULL)
    {
        char msg[256];
        eop_last_error(NULL, msg, sizeof(msg));
        fprintf(stderr, "[probe] eop_connect failed: %s\n", msg);
        return 1;
    }

    if (do_register(client, node_id) != 0)
    {
        eop_disconnect(client);
        return 1;
    }

    /* Signal to the shell script that the initial connection is up */
    printf("REGISTERED\n");
    fflush(stdout);

    /* ── Phase 2: heartbeat loop — wait until the connection breaks ──────── */
    int elapsed_ms = 0;
    int disconnected = 0;

    while (elapsed_ms < HEARTBEAT_WAIT_MAX_MS)
    {
        sleep_ms(HEARTBEAT_INTERVAL_MS);
        elapsed_ms += HEARTBEAT_INTERVAL_MS;

        eop_response_t* hb = eop_send_command(client, EOP_HEARTBEAT, NULL, 0);
        if (hb == NULL)
        {
            char msg[256];
            eop_error_code err = eop_last_error(client, msg, sizeof(msg));
            if (err == EOP_ERR_DISCONNECTED || err == EOP_ERR_TIMEOUT)
            {
                fprintf(stderr, "[probe] Disconnection detected after %d ms: %s\n", elapsed_ms, msg);
                disconnected = 1;
                break;
            }
            /* Any other error is also treated as a lost connection */
            fprintf(stderr, "[probe] Unexpected error during heartbeat: %s\n", msg);
            disconnected = 1;
            break;
        }
        eop_response_free(hb);
    }

    if (!disconnected)
    {
        fprintf(stderr, "[probe] Server did not crash within %d ms — aborting\n", HEARTBEAT_WAIT_MAX_MS);
        eop_disconnect(client);
        return 1;
    }

    /* ── Phase 3+4: reconnect + re-register with combined retry ─────────────
     *
     * eop_reconnect() and the subsequent EOP_REGISTER are retried together.
     * This handles the case where eop_reconnect() succeeds at the TCP level
     * (the port-forward accepts the connection) but the server pod is not yet
     * ready to handle requests and immediately closes the connection, causing
     * do_register() to fail.  Both failures back off and retry the full cycle.
     * ──────────────────────────────────────────────────────────────────────── */
    int delay_ms = RECONNECT_INITIAL_DELAY_MS;
    int attempt = 0;
    int success = 0;

    while (attempt < MAX_RECONNECT_ATTEMPTS)
    {
        attempt++;
        fprintf(stderr, "[probe] Reconnect attempt %d/%d (backoff %d ms)\n", attempt, MAX_RECONNECT_ATTEMPTS, delay_ms);

        eop_error_code rc = eop_reconnect(client, CONNECT_TIMEOUT_MS);
        if (rc != EOP_OK)
        {
            char msg[256];
            eop_last_error(client, msg, sizeof(msg));
            fprintf(stderr, "[probe] eop_reconnect failed: %s\n", msg);
        }
        else if (do_register(client, node_id) == 0)
        {
            success = 1;
            break;
        }
        /* eop_reconnect() succeeded at TCP level but REGISTER failed (server
         * not ready yet) — fall through to backoff and retry the full cycle. */

        int next_delay = delay_ms * RECONNECT_BACKOFF_MULTIPLIER;
        if (next_delay > RECONNECT_MAX_DELAY_MS)
            next_delay = RECONNECT_MAX_DELAY_MS;
        delay_ms = jitter_ms(next_delay);
        sleep_ms(delay_ms);
    }

    if (!success)
    {
        fprintf(stderr, "[probe] exhausted %d attempts — giving up\n", MAX_RECONNECT_ATTEMPTS);
        eop_disconnect(client);
        return 1;
    }

    /* Signal to the shell script that the full recovery cycle is complete */
    printf("RECONNECTED\n");
    fflush(stdout);

    eop_disconnect(client);
    return 0;
}
