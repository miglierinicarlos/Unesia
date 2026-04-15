#!/usr/bin/env bash
# @file pod_auto_restart_test.sh
# @brief Integration test — Pod restart test (Task 6, US-109).
#
# @details Validates that Kubernetes automatically restarts the EOP server
#          container when its process crashes, proving that restartPolicy and
#          the liveness probe are correctly configured.
#
#          This test differs from pod_restart_recovery_flow.sh (Task 5):
#            - Task 5 uses kubectl delete pod (pod-level deletion) and focuses
#              on client reconnect and registration.
#            - Task 6 kills PID 1 inside the running container (process crash)
#              and focuses on Kubernetes restart mechanics.
#
#          Test sequence (run twice to prove reproducibility):
#            1. Verify pod is Running and Ready.
#            2. Record current restart count and timestamp.
#            3. Kill PID 1 inside the container (simulates process crash).
#            4. Wait for the restart count to increment (restartPolicy applied).
#            5. Wait for the pod to return to Running/Ready state.
#            6. Verify elapsed time is within the accepted recovery window.
#
#          Exit codes:
#            0  All assertions passed — automatic restart behavior verified.
#            1  One or more assertions failed — see stderr for details.
#
# Usage:
#   ./tests/integration/pod_auto_restart_test.sh
#
# Prerequisites:
#   - kubectl configured and pointing to a running cluster.
#   - EOP server deployed via k8s/deployment.yaml and k8s/service.yaml.

set -euo pipefail

# ── Constants ─────────────────────────────────────────────────────────────────
readonly APP_LABEL="app=eop-server"
readonly CONTAINER_NAME="eop-server"
readonly NAMESPACE="default"

# Maximum seconds to wait for the container to restart after a crash.
# K8s detects a container exit almost immediately; allowing extra headroom
# for image start-up and probe evaluation.
readonly RESTART_DETECT_TIMEOUT_SECS=60

# Maximum seconds to wait for the pod to return to Ready state after restart.
readonly POD_READY_TIMEOUT_SECS=120

# Poll interval (seconds) when checking restart count.
readonly POLL_INTERVAL_SECS=2

# How many times to run the crash cycle (proves reproducibility AC).
readonly CRASH_CYCLES=2

# Maximum seconds from process crash to pod Ready (AC3 US-109: < 10 s).
readonly MAX_RECOVERY_TIME_SECS=10

# ── Helpers ───────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

pass()  { echo -e "${GREEN}[PASS]${NC} $*"; }
fail()  { echo -e "${RED}[FAIL]${NC} $*" >&2; exit 1; }
step()  { echo -e "${YELLOW}[STEP]${NC} $*"; }
info()  { echo "       $*"; }
cycle() { echo -e "${CYAN}[CYCLE $1/${CRASH_CYCLES}]${NC} $2"; }

# Return the restart count for the first container in the pod selected by APP_LABEL.
get_restart_count() {
    kubectl get pod \
        --namespace "${NAMESPACE}" \
        --selector "${APP_LABEL}" \
        --output=jsonpath='{.items[0].status.containerStatuses[0].restartCount}' \
        2>/dev/null || echo "0"
}

# Return the name of the first pod selected by APP_LABEL.
get_pod_name() {
    kubectl get pod \
        --namespace "${NAMESPACE}" \
        --selector "${APP_LABEL}" \
        --output=jsonpath='{.items[0].metadata.name}'
}

# Return the container ID (runtime prefix stripped) for the target container.
# kubectl returns values like "containerd://abc123" or "docker://abc123".
get_container_id() {
    kubectl get pod \
        --namespace "${NAMESPACE}" \
        --selector "${APP_LABEL}" \
        --output=jsonpath='{.items[0].status.containerStatuses[0].containerID}' \
        2>/dev/null | sed 's|.*://||'
}

# Wait until the restart count for the pod exceeds $1, or timeout.
wait_for_restart() {
    local expected_min_count=$1
    local elapsed=0

    while [[ ${elapsed} -lt ${RESTART_DETECT_TIMEOUT_SECS} ]]; do
        local count
        count=$(get_restart_count)
        if [[ "${count}" -ge "${expected_min_count}" ]]; then
            info "Restart count reached ${count} (expected ≥ ${expected_min_count}) after ${elapsed}s"
            return 0
        fi
        sleep "${POLL_INTERVAL_SECS}"
        elapsed=$(( elapsed + POLL_INTERVAL_SECS ))
    done

    local final_count
    final_count=$(get_restart_count)
    fail "Restart count did not reach ${expected_min_count} within ${RESTART_DETECT_TIMEOUT_SECS}s (current: ${final_count})"
}

# ── Prerequisites ─────────────────────────────────────────────────────────────
step "Checking prerequisites"

command -v kubectl  >/dev/null 2>&1 || fail "kubectl not found — install and configure it first"
command -v minikube >/dev/null 2>&1 || fail "minikube not found — required to kill PID 1 in distroless containers"

if ! kubectl cluster-info --request-timeout=5s >/dev/null 2>&1; then
    fail "kubectl cannot reach the cluster — check your kubeconfig"
fi

pass "Prerequisites OK"

# ── Verify initial pod state ───────────────────────────────────────────────────
step "Verifying initial pod state — Running and Ready"

if ! kubectl wait pod \
        --namespace "${NAMESPACE}" \
        --selector "${APP_LABEL}" \
        --for=condition=Ready \
        --timeout="${POD_READY_TIMEOUT_SECS}s" \
        >/dev/null 2>&1; then
    kubectl get pods --namespace "${NAMESPACE}" --selector "${APP_LABEL}" >&2
    fail "EOP server pod is not Ready — deploy k8s/ manifests first"
fi

INITIAL_RESTART_COUNT=$(get_restart_count)
info "Pod: $(get_pod_name)"
info "Current restart count: ${INITIAL_RESTART_COUNT}"

pass "Initial pod state OK"

# ── Verify restartPolicy is Always ────────────────────────────────────────────
step "Verifying restartPolicy is set to Always"

RESTART_POLICY=$(kubectl get pod \
    --namespace "${NAMESPACE}" \
    --selector "${APP_LABEL}" \
    --output=jsonpath='{.items[0].spec.restartPolicy}')

if [[ "${RESTART_POLICY}" != "Always" ]]; then
    fail "restartPolicy is '${RESTART_POLICY}' — expected 'Always' (k8s/deployment.yaml)"
fi

info "restartPolicy: ${RESTART_POLICY}"
pass "restartPolicy correctly set to Always"

# ── Verify liveness probe is configured ───────────────────────────────────────
step "Verifying liveness probe is configured"

LIVENESS_PORT=$(kubectl get pod \
    --namespace "${NAMESPACE}" \
    --selector "${APP_LABEL}" \
    --output=jsonpath="{.items[0].spec.containers[?(@.name==\"${CONTAINER_NAME}\")].livenessProbe.tcpSocket.port}")

if [[ -z "${LIVENESS_PORT}" ]]; then
    fail "No tcpSocket liveness probe found on container '${CONTAINER_NAME}'"
fi

info "Liveness probe: tcpSocket port=${LIVENESS_PORT}"
pass "Liveness probe configured"

# ── Reset pod to eliminate accumulated CrashLoopBackOff back-off ─────────────
# If the pod has prior restarts from earlier test runs, Kubernetes applies
# exponential back-off (0s → 10s → 20s → 40s → 80s …) before restarting.
# At restartCount=4 the back-off is ~80s, which exceeds RESTART_DETECT_TIMEOUT_SECS
# and also breaks the AC3 recovery-time assertion (< 10s) on cycle 1.
# A rollout restart produces a new pod (restartCount=0, no back-off) so that
# cycle 1 measures a clean, unpenalised crash-to-Ready round-trip.
step "Resetting pod state to clear any accumulated CrashLoopBackOff back-off"

if [[ "${INITIAL_RESTART_COUNT}" -gt 0 ]]; then
    # Capture the current pod name so we can wait for it to terminate after the
    # rollout restart — without this wait, get_pod_name() may return the old
    # (Terminating) pod and get_container_id() will produce a stale ID that
    # docker kill can no longer reach.
    PRE_RESET_POD=$(get_pod_name)

    if ! kubectl rollout restart deployment/eop-server \
            --namespace "${NAMESPACE}" >/dev/null 2>&1; then
        fail "kubectl rollout restart failed — cannot reset pod state"
    fi

    if ! kubectl rollout status deployment/eop-server \
            --namespace "${NAMESPACE}" \
            --timeout="${POD_READY_TIMEOUT_SECS}s" >/dev/null 2>&1; then
        fail "Deployment did not finish rolling out within ${POD_READY_TIMEOUT_SECS}s"
    fi

    # Wait for the old pod to fully disappear before querying the new one.
    # kubectl rollout status returns as soon as the new pod is Ready, but the
    # old pod may still exist for a few seconds.  If we skip this wait,
    # get_pod_name() can return the Terminating pod whose container ID is
    # already invalid for docker/crictl kill.
    kubectl wait pod \
        --namespace "${NAMESPACE}" \
        "${PRE_RESET_POD}" \
        --for=delete \
        --timeout="${POD_READY_TIMEOUT_SECS}s" \
        >/dev/null 2>&1 || true

    # Re-read state from the freshly started pod.
    INITIAL_RESTART_COUNT=$(get_restart_count)
    info "Pod: $(get_pod_name)"
    info "Restart count after reset: ${INITIAL_RESTART_COUNT}"
    pass "Pod reset complete — fresh pod with no accumulated back-off"
else
    info "Pod: $(get_pod_name)"
    info "Restart count is already 0 — no rollout restart needed"
    pass "Pod reset complete — no accumulated back-off detected"
fi

# ── Crash cycles ──────────────────────────────────────────────────────────────
EXPECTED_RESTART_COUNT=${INITIAL_RESTART_COUNT}

for cycle_num in $(seq 1 "${CRASH_CYCLES}"); do

    cycle "${cycle_num}" "Starting crash cycle"

    # ── Record state before crash ──────────────────────────────────────────────
    POD_NAME=$(get_pod_name)
    PRE_CRASH_COUNT=$(get_restart_count)
    EXPECTED_RESTART_COUNT=$(( PRE_CRASH_COUNT + 1 ))
    CRASH_START_SECS="${SECONDS}"

    info "Pod before crash: ${POD_NAME}"
    info "Restart count before crash: ${PRE_CRASH_COUNT}"

    # ── Simulate process crash via node-level SIGKILL ─────────────────────────
    # The runtime image is distroless (gcr.io/distroless/cc-debian12) — it has
    # no shell or kill utility. kubectl exec cannot be used to signal PID 1.
    # Instead we retrieve the container ID and send SIGKILL from the minikube
    # node via crictl (containerd runtime) or docker (docker driver fallback).
    cycle "${cycle_num}" "Crashing container '${CONTAINER_NAME}' via node-level SIGKILL (distroless — no shell in image)"

    CONTAINER_ID=$(get_container_id)
    if [[ -z "${CONTAINER_ID}" ]]; then
        fail "Could not determine container ID for pod ${POD_NAME}"
    fi
    info "Container ID: ${CONTAINER_ID}"

    if ! minikube ssh "sudo crictl kill --signal SIGKILL ${CONTAINER_ID} 2>/dev/null || \
                       sudo docker kill --signal SIGKILL ${CONTAINER_ID} 2>/dev/null"; then
        fail "Failed to send SIGKILL to container ${CONTAINER_ID} via minikube node"
    fi

    info "SIGKILL sent to container ${CONTAINER_ID} via minikube node — process terminated"

    # ── Wait for restart count to increment ───────────────────────────────────
    cycle "${cycle_num}" "Waiting for Kubernetes to restart the container (restartPolicy=Always)"

    wait_for_restart "${EXPECTED_RESTART_COUNT}"

    DETECT_ELAPSED=$(( SECONDS - CRASH_START_SECS ))
    info "Container restart detected after ${DETECT_ELAPSED}s"

    # ── Wait for pod to return to Ready state ─────────────────────────────────
    cycle "${cycle_num}" "Waiting for pod to return to Ready state"

    if ! kubectl wait pod \
            --namespace "${NAMESPACE}" \
            --selector "${APP_LABEL}" \
            --for=condition=Ready \
            --timeout="${POD_READY_TIMEOUT_SECS}s" \
            >/dev/null 2>&1; then
        kubectl get pods --namespace "${NAMESPACE}" --selector "${APP_LABEL}" >&2
        fail "Pod did not return to Ready state within ${POD_READY_TIMEOUT_SECS}s after crash cycle ${cycle_num}"
    fi

    TOTAL_ELAPSED=$(( SECONDS - CRASH_START_SECS ))
    POST_CRASH_COUNT=$(get_restart_count)

    info "Pod Ready again after ${TOTAL_ELAPSED}s"
    info "Restart count after crash: ${POST_CRASH_COUNT}"

    # ── Assert restart count increased ────────────────────────────────────────
    if [[ "${POST_CRASH_COUNT}" -lt "${EXPECTED_RESTART_COUNT}" ]]; then
        fail "Restart count did not increment: expected ≥ ${EXPECTED_RESTART_COUNT}, got ${POST_CRASH_COUNT}"
    fi

    # ── Assert recovery time < 10 s — cycle 1 only (AC3 US-109) ──────────────
    # Kubernetes applies CrashLoopBackOff exponential back-off on subsequent
    # crashes (0s → 10s → 20s → ...). The AC3 requirement covers a single crash
    # event with no prior back-off accumulated. Cycles > 1 prove reproducibility
    # (K8s restarts again) but are not subject to the timing constraint.
    if [[ ${cycle_num} -eq 1 && ${TOTAL_ELAPSED} -ge ${MAX_RECOVERY_TIME_SECS} ]]; then
        fail "Recovery time ${TOTAL_ELAPSED}s exceeded limit of ${MAX_RECOVERY_TIME_SECS}s (AC3 US-109)"
    fi

    if [[ ${cycle_num} -eq 1 ]]; then
        pass "Cycle ${cycle_num}/${CRASH_CYCLES}: pod restarted and is Ready in ${TOTAL_ELAPSED}s (limit: ${MAX_RECOVERY_TIME_SECS}s, restartCount=${POST_CRASH_COUNT})"
    else
        pass "Cycle ${cycle_num}/${CRASH_CYCLES}: pod restarted and is Ready in ${TOTAL_ELAPSED}s (CrashLoopBackOff back-off included — no time limit, restartCount=${POST_CRASH_COUNT})"
    fi

done

# ── Result ────────────────────────────────────────────────────────────────────
FINAL_COUNT=$(get_restart_count)
TOTAL_CRASHES=$(( FINAL_COUNT - INITIAL_RESTART_COUNT ))

echo ""
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${GREEN}  pod_auto_restart_test: PASS                                    ${NC}"
echo -e "${GREEN}  restartPolicy=Always verified — pod restarted ${TOTAL_CRASHES}x after crash  ${NC}"
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
