#!/usr/bin/env bash

# Local Trace Validation Script
# This simple tool pings the EOP-Server to generate observability traces
# into the active OpenTelemetry collector stack.
#
# Usage: ./trace_validation.sh

set -e

SERVER_HOST="localhost"
SERVER_PORT=9026
NUM_REQUESTS=${1:-5}

echo "========================================="
echo "🧪 Running Smoke Trace Validation Test"
echo "========================================="

if ! command -v nc &> /dev/null; then
    echo "Error: 'nc' (netcat) command could not be found. Please install it."
    exit 1
fi

echo "[*] Sending $NUM_REQUESTS connections to $SERVER_HOST:$SERVER_PORT..."

for i in $(seq 1 $NUM_REQUESTS); do
    echo "[-] Sending request $i/$NUM_REQUESTS"
    echo "PING test-$i" | nc -w 1 $SERVER_HOST $SERVER_PORT || true
    sleep 0.5
done

echo ""
echo "✅ Finished sending data!"
echo "[*] You can verify the traces in SigNoz by opening: http://localhost:8080/traces-explorer"
echo "[*] You can verify the logs by running: docker compose logs eop-server"
