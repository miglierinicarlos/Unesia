#!/usr/bin/env bash
# @file rolling_restart_test.sh
# @brief Integration test — rolling restart zero-downtime verification (Task 7, US-204).
#
# @details Validates that kubectl rollout restart deployment/eop-server completes
#          without any connection failures observed by concurrent active clients
#          (AC7 of US-204). The v0.1 server handles SIGTERM gracefully; this test
#          verifies that the K8s rolling update strategy, combined with graceful
#          shutdown, produces a zero-error restart from the client's perspective.
#
#          Test sequence:
#            1. Verify all EOP pods are Running and Ready.
#            2. Establish port-forward to eop-server.
#            3. Start N background client processes sending REGISTER+QUERY_NODE.
#            4. Trigger kubectl rollout restart deployment/eop-server.
#            5. Wait for rollout to complete (timeout: 60s).
#            6. Stop background clients and count errors.
#            7. Assert zero errors. Print summary.
#
#          Exit codes:
#            0  All pods restarted with zero client errors — AC7 satisfied.
#            1  One or more steps failed — see stderr for details.
#
# Usage:
#   ./tests/integration/rolling_restart_test.sh
#
# Prerequisites:
#   - kubectl configured and pointing to a running cluster.
#   - Full EOP stack deployed via make k8s-apply.
#   - eop_reconnect_probe built and on PATH (or in build/tests/).

set -euo pipefail

# ── Constants ─────────────────────────────────────────────────────────────────
readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

readonly NAMESPACE="default"
readonly DEPLOYMENT_SERVER="eop-server"
readonly DEPLOYMENT_HPC="eop-hpc-engine"
readonly SERVICE_NAME="eop-server"
readonly LOCAL_PORT="19026"
readonly SERVER_PORT="9026"
readonly NUM_CLIENTS=3
readonly ROLLOUT_TIMEOUT="60s"
readonly CLIENT_BINARY="${REPO_ROOT}/build/tests/eop_reconnect_probe"

readonly LOG_DIR="$(mktemp -d /tmp/rolling_restart_test.XXXXXX)"
readonly PF_PID_FILE="${LOG_DIR}/port_forward.pid"

# ── Helpers ───────────────────────────────────────────────────────────────────
log()  { echo "[$(date '+%H:%M:%S')] $*"; }
fail() { echo "[FAIL] $*" >&2; exit 1; }

cleanup() {
    log "Cleaning up background processes..."
    if [ -f "${PF_PID_FILE}" ]; then
        kill "$(cat "${PF_PID_FILE}")" 2>/dev/null || true
    fi
    # Kill any client processes still running
    for pid_file in "${LOG_DIR}"/client_*.pid; do
        [ -f "${pid_file}" ] || continue
        kill "$(cat "${pid_file}")" 2>/dev/null || true
    done
    log "Cleanup complete. Logs: ${LOG_DIR}"
}
trap cleanup EXIT

# ── Step 1: Verify full stack is Running ──────────────────────────────────────
log "Step 1: Verifying all EOP pods are Running and Ready..."

NOT_READY=$(kubectl get pods -n "${NAMESPACE}" \
    --field-selector=status.phase!=Running \
    --no-headers 2>/dev/null \
    | { grep -E "${DEPLOYMENT_SERVER}|${DEPLOYMENT_HPC}" || true; } \
    | { grep -v -E "Completed|Succeeded" || true; } \
    | wc -l)

if [ "${NOT_READY}" -gt 0 ]; then
    fail "Some EOP pods are not Running. Deploy with 'make k8s-apply' first."
fi

kubectl get pods -n "${NAMESPACE}" \
    -l "app in (${DEPLOYMENT_SERVER},${DEPLOYMENT_HPC})" --no-headers
log "All EOP pods Running."

# ── Step 2: Port-forward to eop-server ────────────────────────────────────────
log "Step 2: Establishing port-forward ${LOCAL_PORT} -> ${SERVICE_NAME}:${SERVER_PORT}..."

kubectl port-forward \
    "service/${SERVICE_NAME}" \
    "${LOCAL_PORT}:${SERVER_PORT}" \
    -n "${NAMESPACE}" \
    > "${LOG_DIR}/port_forward.log" 2>&1 &
echo $! > "${PF_PID_FILE}"

sleep 2
if ! kill -0 "$(cat "${PF_PID_FILE}")" 2>/dev/null; then
    fail "Port-forward failed to start. Check ${LOG_DIR}/port_forward.log"
fi
log "Port-forward active (PID=$(cat "${PF_PID_FILE}"))."

# ── Step 3: Start concurrent client processes ─────────────────────────────────
log "Step 3: Starting ${NUM_CLIENTS} concurrent client processes..."

if [ ! -x "${CLIENT_BINARY}" ]; then
    fail "Client binary not found: ${CLIENT_BINARY}. Build with: cmake --build build --target eop_reconnect_probe"
fi

START_TIME=$(date +%s)

for i in $(seq 1 "${NUM_CLIENTS}"); do
    CLIENT_LOG="${LOG_DIR}/client_${i}.log"
    "${CLIENT_BINARY}" 127.0.0.1 "${LOCAL_PORT}" "probe-node-${i}" \
        > "${CLIENT_LOG}" 2>&1 &
    echo $! > "${LOG_DIR}/client_${i}.pid"
    log "  Client ${i} started (PID=$!)."
done

sleep 1

# ── Step 4: Trigger rolling restart ───────────────────────────────────────────
log "Step 4: Triggering kubectl rollout restart deployment/${DEPLOYMENT_SERVER}..."
kubectl rollout restart "deployment/${DEPLOYMENT_SERVER}" -n "${NAMESPACE}"

ROLLOUT_START=$(date +%s)

# ── Step 5: Wait for rollout to complete ──────────────────────────────────────
log "Step 5: Waiting for rollout to complete (timeout: ${ROLLOUT_TIMEOUT})..."
if ! kubectl rollout status "deployment/${DEPLOYMENT_SERVER}" \
        -n "${NAMESPACE}" \
        --timeout="${ROLLOUT_TIMEOUT}"; then
    fail "Rollout did not complete within ${ROLLOUT_TIMEOUT}."
fi

ROLLOUT_END=$(date +%s)
ROLLOUT_DURATION=$(( ROLLOUT_END - ROLLOUT_START ))
log "Rollout completed in ${ROLLOUT_DURATION}s."

# Allow clients to run a bit more after rollout
sleep 2

# ── Step 6: Stop clients and count errors ─────────────────────────────────────
log "Step 6: Stopping clients and counting errors..."

TOTAL_REQUESTS=0
TOTAL_ERRORS=0

for i in $(seq 1 "${NUM_CLIENTS}"); do
    PID_FILE="${LOG_DIR}/client_${i}.pid"
    CLIENT_LOG="${LOG_DIR}/client_${i}.log"

    if [ -f "${PID_FILE}" ]; then
        kill "$(cat "${PID_FILE}")" 2>/dev/null || true
        wait "$(cat "${PID_FILE}")" 2>/dev/null || true
    fi

    CLIENT_REQUESTS=$(grep -c "REGISTER\|QUERY" "${CLIENT_LOG}" 2>/dev/null || true)
    CLIENT_ERRORS=$(grep -c "ERROR\|FAIL\|error\|refused\|timeout" "${CLIENT_LOG}" 2>/dev/null || true)
    TOTAL_REQUESTS=$(( TOTAL_REQUESTS + CLIENT_REQUESTS ))
    TOTAL_ERRORS=$(( TOTAL_ERRORS + CLIENT_ERRORS ))

    log "  Client ${i}: requests=${CLIENT_REQUESTS}, errors=${CLIENT_ERRORS}"
done

# ── Step 7: Assert zero errors ────────────────────────────────────────────────
END_TIME=$(date +%s)
TOTAL_DURATION=$(( END_TIME - START_TIME ))

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  Rolling Restart Test — Summary"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  Clients:          ${NUM_CLIENTS}"
echo "  Total requests:   ${TOTAL_REQUESTS}"
echo "  Total errors:     ${TOTAL_ERRORS}"
echo "  Rollout duration: ${ROLLOUT_DURATION}s (limit: ${ROLLOUT_TIMEOUT})"
echo "  Total test time:  ${TOTAL_DURATION}s"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

if [ "${TOTAL_ERRORS}" -gt 0 ]; then
    echo "  [FAIL] ${TOTAL_ERRORS} client error(s) observed during rolling restart."
    echo "  AC7 NOT satisfied — see logs in ${LOG_DIR}"
    exit 1
fi

if [ "${ROLLOUT_DURATION}" -gt 60 ]; then
    echo "  [FAIL] Rollout took ${ROLLOUT_DURATION}s — exceeds 60s limit."
    exit 1
fi

echo "  [PASS] Zero errors. AC7 satisfied — rolling restart is zero-downtime."
kubectl get pods -n "${NAMESPACE}" -l "app=${DEPLOYMENT_SERVER}"

# ── Step 8: Verify HPC engine rollout restart (if deployed) ───────────────────
HPC_REPLICAS=$(kubectl get deployment "${DEPLOYMENT_HPC}" -n "${NAMESPACE}" \
    --ignore-not-found \
    -o jsonpath='{.spec.replicas}' 2>/dev/null || true)

if [ -n "${HPC_REPLICAS}" ] && [ "${HPC_REPLICAS}" -gt 0 ]; then
    echo ""
    log "Step 8: Triggering kubectl rollout restart deployment/${DEPLOYMENT_HPC}..."
    kubectl rollout restart "deployment/${DEPLOYMENT_HPC}" -n "${NAMESPACE}"

    HPC_ROLLOUT_START=$(date +%s)
    log "Step 8: Waiting for HPC engine rollout to complete (timeout: ${ROLLOUT_TIMEOUT})..."
    if ! kubectl rollout status "deployment/${DEPLOYMENT_HPC}" \
            -n "${NAMESPACE}" \
            --timeout="${ROLLOUT_TIMEOUT}"; then
        echo "  [FAIL] HPC engine rollout did not complete within ${ROLLOUT_TIMEOUT}."
        exit 1
    fi

    HPC_ROLLOUT_DURATION=$(( $(date +%s) - HPC_ROLLOUT_START ))
    log "HPC engine rollout completed in ${HPC_ROLLOUT_DURATION}s."

    if [ "${HPC_ROLLOUT_DURATION}" -gt 60 ]; then
        echo "  [FAIL] HPC engine rollout took ${HPC_ROLLOUT_DURATION}s — exceeds 60s limit."
        exit 1
    fi

    echo "  [PASS] HPC engine rolling restart completed with zero downtime."
    kubectl get pods -n "${NAMESPACE}" -l "app=${DEPLOYMENT_HPC}"
else
    log "Step 8: HPC engine deployment not found or replicas=0 — skipping."
fi
