#!/usr/bin/env bash
# @file pod_restart_recovery_flow.sh
# @brief Integration test — pod_restart_recovery_flow (Task 5, US-109).
#
# @details Validates the full pod restart and client recovery cycle against a
#          live Kubernetes cluster using the real C client library
#          (eop_reconnect_probe), exercising eop_reconnect() end-to-end per
#          ADR-015 and AC6/AC7 of US-109.
#
#          Test sequence:
#            1. Verify the EOP server pod is Running and Ready.
#            2. Start eop_reconnect_probe in background; wait for "REGISTERED".
#            3. Delete the pod to trigger Kubernetes automatic restart.
#            4. Wait for the replacement pod to reach Running/Ready state.
#            5. Re-establish a port-forward to the restarted pod.
#            6. Wait for the probe to print "RECONNECTED" (eop_reconnect() +
#               re-registration via the C library — no raw Python socket).
#
#          Exit codes:
#            0  All steps passed — full restart-recovery cycle verified.
#            1  One or more steps failed — see stderr for details.
#
# Usage:
#   ./tests/integration/pod_restart_recovery_flow.sh
#
# Prerequisites:
#   - kubectl configured and pointing to a running cluster.
#   - EOP server deployed via k8s/deployment.yaml and k8s/service.yaml.
#   - eop_reconnect_probe built and on PATH (or in build/tests/).
#     Build: cmake --build <build-dir> --target eop_reconnect_probe

set -euo pipefail

# ── Constants ─────────────────────────────────────────────────────────────────
readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

readonly APP_LABEL="app=eop-server"
readonly NAMESPACE="default"
readonly SERVICE_NAME="eop-server"
readonly SERVICE_PORT=9026
readonly LOCAL_PORT=19099
readonly POD_READY_TIMEOUT_SECS=120
readonly PORT_FORWARD_SETTLE_MS=800
readonly REGISTER_NODE_ID="vault-109-k8s-recovery"
# Maximum acceptable time (seconds) from pod deletion to replacement pod Ready.
# Covers: K8s scheduling + container start + readiness probe initialDelaySeconds (2s).
readonly MAX_RESTART_TIME_SECS=30
# Timeout (seconds) for the probe to report RECONNECTED after the pod is Ready.
# Must exceed MAX_RECONNECT_ATTEMPTS * MAX_DELAY (10 * 5s = 50s) in the worst case.
readonly PROBE_RECONNECT_TIMEOUT_SECS=60

# ── Helpers ───────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass()  { echo -e "${GREEN}[PASS]${NC} $*"; }
fail()  { echo -e "${RED}[FAIL]${NC} $*" >&2; exit 1; }
step()  { echo -e "${YELLOW}[STEP]${NC} $*"; }
info()  { echo "       $*"; }

# PIDs managed by this script.
PF_PID=""     # kubectl port-forward background process
PROBE_PID=""  # eop_reconnect_probe background process
PROBE_LOG=""  # temp file receiving probe stdout ("REGISTERED" / "RECONNECTED")

# Kill background processes and remove the temp log on exit.
cleanup() {
    if [[ -n "${PF_PID}" ]] && kill -0 "${PF_PID}" 2>/dev/null; then
        kill "${PF_PID}" 2>/dev/null || true
        wait "${PF_PID}" 2>/dev/null || true
    fi
    if [[ -n "${PROBE_PID}" ]] && kill -0 "${PROBE_PID}" 2>/dev/null; then
        kill "${PROBE_PID}" 2>/dev/null || true
        wait "${PROBE_PID}" 2>/dev/null || true
    fi
    if [[ -n "${PROBE_LOG}" ]]; then
        rm -f "${PROBE_LOG}"
    fi
}
trap cleanup EXIT

# Start kubectl port-forward in the background.
# Sets PF_PID to the background process ID.
# Waits PORT_FORWARD_SETTLE_MS milliseconds for the tunnel to be established.
start_port_forward() {
    # Kill the port-forward we own, if any.
    if [[ -n "${PF_PID}" ]] && kill -0 "${PF_PID}" 2>/dev/null; then
        kill "${PF_PID}" 2>/dev/null || true
        wait "${PF_PID}" 2>/dev/null || true
        PF_PID=""
    fi

    # Kill any stale kubectl port-forward that is still LISTENING on the port.
    # Using -sTCP:LISTEN ensures we only match server-side (listening) sockets
    # and never accidentally kill the probe, which connects as a client and may
    # still hold a CLOSE_WAIT socket on the same port after the pod restarts.
    local stale_pids
    stale_pids=$(lsof -ti :"${LOCAL_PORT}" -sTCP:LISTEN 2>/dev/null || true)
    if [[ -n "${stale_pids}" ]]; then
        info "Killing stale process(es) listening on port ${LOCAL_PORT}: ${stale_pids}"
        echo "${stale_pids}" | xargs kill 2>/dev/null || true
        sleep 0.3
    fi

    local pf_log
    pf_log="$(mktemp /tmp/eop_pf_log.XXXXXX)"

    kubectl port-forward \
        --namespace "${NAMESPACE}" \
        "svc/${SERVICE_NAME}" \
        "${LOCAL_PORT}:${SERVICE_PORT}" \
        >"${pf_log}" 2>&1 &
    PF_PID=$!

    sleep "$(awk "BEGIN {printf \"%.3f\", ${PORT_FORWARD_SETTLE_MS} / 1000}")"

    if ! kill -0 "${PF_PID}" 2>/dev/null; then
        fail "kubectl port-forward failed to start: $(cat "${pf_log}")"
    fi
    rm -f "${pf_log}"
}

# Locate the eop_reconnect_probe binary.
# Searches PATH first, then common CMake build directories.
find_probe() {
    if command -v eop_reconnect_probe >/dev/null 2>&1; then
        echo "eop_reconnect_probe"
        return 0
    fi
    for candidate in \
        "${REPO_ROOT}/build/tests/eop_reconnect_probe" \
        "${REPO_ROOT}/build-tsan/tests/eop_reconnect_probe" \
        "${REPO_ROOT}/cmake-build-debug/tests/eop_reconnect_probe" \
        "${REPO_ROOT}/cmake-build-release/tests/eop_reconnect_probe"
    do
        if [[ -x "${candidate}" ]]; then
            echo "${candidate}"
            return 0
        fi
    done
    return 1
}

# Wait up to $2 seconds for $1 to appear as a line in PROBE_LOG.
# Polls the regular file once per second — no FIFO blocking issues.
# Returns 0 if the line was seen, 1 on timeout.
wait_for_probe_line() {
    local expected_line="$1"
    local timeout_secs="$2"
    local elapsed=0

    while [[ ${elapsed} -lt ${timeout_secs} ]]; do
        if grep -qF "${expected_line}" "${PROBE_LOG}" 2>/dev/null; then
            return 0
        fi
        sleep 1
        elapsed=$(( elapsed + 1 ))
    done
    return 1
}

# ── Prerequisites ─────────────────────────────────────────────────────────────
step "Checking prerequisites"

command -v kubectl >/dev/null 2>&1 || fail "kubectl not found — install and configure it first"

if ! kubectl cluster-info --request-timeout=5s >/dev/null 2>&1; then
    fail "kubectl cannot reach the cluster — check your kubeconfig"
fi

PROBE_BIN=""
if ! PROBE_BIN=$(find_probe); then
    fail "eop_reconnect_probe not found — build it first: cmake --build <build-dir> --target eop_reconnect_probe"
fi
info "Using probe: ${PROBE_BIN}"

pass "Prerequisites OK"

# ── Step 1: Verify pod is Running and Ready ───────────────────────────────────
step "Step 1: Verify EOP server pod is Running and Ready"

if ! kubectl wait pod \
        --namespace "${NAMESPACE}" \
        --selector "${APP_LABEL}" \
        --for=condition=Ready \
        --timeout="${POD_READY_TIMEOUT_SECS}s" \
        >/dev/null 2>&1; then
    kubectl get pods --namespace "${NAMESPACE}" --selector "${APP_LABEL}" >&2
    fail "EOP server pod is not Ready — deploy k8s/ manifests first"
fi

INITIAL_POD=$(kubectl get pod \
    --namespace "${NAMESPACE}" \
    --selector "${APP_LABEL}" \
    --output=jsonpath='{.items[0].metadata.name}')
info "Pod: ${INITIAL_POD}"

pass "Step 1 OK — pod Ready"

# ── Step 2: Start eop_reconnect_probe and wait for initial REGISTER ───────────
step "Step 2: Start eop_reconnect_probe — initial connect + REGISTER via C library"

start_port_forward
info "Port-forward: localhost:${LOCAL_PORT} → svc/${SERVICE_NAME}:${SERVICE_PORT} (PID ${PF_PID})"

# stdout ("REGISTERED" / "RECONNECTED") goes to a regular temp file — no buffer
# limits and no blocking on open. stderr (heartbeat errors, retry attempts) is
# sent to the controlling terminal if one is available (/dev/tty), or to the
# script's own stderr otherwise (e.g. when invoked via make without a TTY).
PROBE_LOG="$(mktemp /tmp/eop_probe_log.XXXXXX)"

# Probe if /dev/tty can actually be opened for writing (not just that the file
# exists and has write permission bits set).  When the script is invoked via
# make or another non-interactive parent there is no controlling terminal, so
# open(2) on /dev/tty fails with ENXIO even if -w returns true.
PROBE_STDERR_TARGET=/dev/stderr
if { true 2>/dev/null >/dev/tty; } 2>/dev/null; then
    PROBE_STDERR_TARGET=/dev/tty
fi

"${PROBE_BIN}" "127.0.0.1" "${LOCAL_PORT}" "${REGISTER_NODE_ID}" \
    >"${PROBE_LOG}" 2>"${PROBE_STDERR_TARGET}" &
PROBE_PID=$!
info "eop_reconnect_probe started (PID ${PROBE_PID})"

if ! wait_for_probe_line "REGISTERED" 15; then
    fail "eop_reconnect_probe did not print REGISTERED within 15 s"
fi

pass "Step 2 OK — initial REGISTER via eop_connect() + eop_send_command() succeeded"

# ── Step 3: Delete the pod to trigger automatic restart ───────────────────────
step "Step 3: Delete pod '${INITIAL_POD}' to trigger Kubernetes restart"

# Record wall-clock time before deletion so we can measure total restart latency.
RESTART_START_MS=$(date +%s%3N)

kubectl delete pod \
    --namespace "${NAMESPACE}" \
    "${INITIAL_POD}" \
    --wait=false \
    >/dev/null

info "Pod deletion requested — Kubernetes will restart it automatically"
pass "Step 3 OK — pod deletion sent"

# ── Step 4: Wait for replacement pod to be Running and Ready ──────────────────
step "Step 4: Wait for replacement pod to reach Ready state (timeout ${POD_READY_TIMEOUT_SECS}s)"

# First wait for the old pod to disappear to avoid matching it again.
kubectl wait pod \
    --namespace "${NAMESPACE}" \
    "${INITIAL_POD}" \
    --for=delete \
    --timeout="${POD_READY_TIMEOUT_SECS}s" \
    >/dev/null 2>&1 || true

# Now wait for the replacement pod to become Ready.
if ! kubectl wait pod \
        --namespace "${NAMESPACE}" \
        --selector "${APP_LABEL}" \
        --for=condition=Ready \
        --timeout="${POD_READY_TIMEOUT_SECS}s" \
        >/dev/null 2>&1; then
    kubectl get pods --namespace "${NAMESPACE}" --selector "${APP_LABEL}" >&2
    fail "Replacement pod did not reach Ready state within ${POD_READY_TIMEOUT_SECS}s"
fi

RESTART_END_MS=$(date +%s%3N)
RESTART_ELAPSED_MS=$((RESTART_END_MS - RESTART_START_MS))
RESTART_ELAPSED_S=$(awk "BEGIN {printf \"%.2f\", ${RESTART_ELAPSED_MS} / 1000}")

NEW_POD=$(kubectl get pod \
    --namespace "${NAMESPACE}" \
    --selector "${APP_LABEL}" \
    --output=jsonpath='{.items[0].metadata.name}')
info "Replacement pod: ${NEW_POD}"
info "Recovery time: ${RESTART_ELAPSED_S}s (crash-to-Ready, limit: ${MAX_RESTART_TIME_SECS}s)"

if [[ ${RESTART_ELAPSED_MS} -gt $((MAX_RESTART_TIME_SECS * 1000)) ]]; then
    fail "Recovery time ${RESTART_ELAPSED_S}s exceeded limit of ${MAX_RESTART_TIME_SECS}s"
fi

pass "Step 4 OK — replacement pod Ready in ${RESTART_ELAPSED_S}s"

# ── Step 5: Re-establish port-forward after pod restart ───────────────────────
step "Step 5: Re-establish port-forward to restarted pod"

start_port_forward
info "Port-forward: localhost:${LOCAL_PORT} → svc/${SERVICE_NAME}:${SERVICE_PORT} (PID ${PF_PID})"

pass "Step 5 OK — port-forward re-established"

# ── Step 6: Wait for eop_reconnect_probe to complete the recovery cycle ───────
step "Step 6: Wait for eop_reconnect_probe — eop_reconnect() + re-register via C library"
info "Probe (PID ${PROBE_PID}) is retrying eop_reconnect() with exponential backoff (ADR-015)"

if ! wait_for_probe_line "RECONNECTED" "${PROBE_RECONNECT_TIMEOUT_SECS}"; then
    # Give the probe stderr a moment to flush, then capture it for diagnostics
    wait "${PROBE_PID}" 2>/dev/null || true
    fail "eop_reconnect_probe did not print RECONNECTED within ${PROBE_RECONNECT_TIMEOUT_SECS}s — eop_reconnect() failed"
fi

# Wait for the probe to exit cleanly (it exits 0 right after printing RECONNECTED)
wait "${PROBE_PID}"
PROBE_EXIT=$?
PROBE_PID=""

if [[ ${PROBE_EXIT} -ne 0 ]]; then
    fail "eop_reconnect_probe exited with code ${PROBE_EXIT} after RECONNECTED — re-registration failed"
fi

pass "Step 6 OK — eop_reconnect() + re-registration via C library succeeded"

# ── Result ────────────────────────────────────────────────────────────────────
echo ""
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${GREEN}  pod_restart_recovery_flow: PASS                                   ${NC}"
echo -e "${GREEN}  Full restart-and-recovery cycle verified end-to-end               ${NC}"
echo -e "${GREEN}  eop_reconnect() exercised via C library (eop_reconnect_probe)     ${NC}"
echo -e "${GREEN}  Recovery time (crash-to-Ready): ${RESTART_ELAPSED_S}s (limit: ${MAX_RESTART_TIME_SECS}s)          ${NC}"
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
