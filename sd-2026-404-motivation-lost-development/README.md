# Exodus Ops Platform (EOP)

Cloud-native distributed systems platform for Vault-Tec's bunker infrastructure.
EOP enables real-time coordination, processing, and observation of distributed
nodes through a custom TCP protocol, containerized deployment, and full
distributed tracing.

*Current release: v0.1 — Core Communication Layer*

## Architecture

```text
 ┌──────────────┐
 │   C Client   │
 │   Library    │
 └─┬──────────▲─┘
   │          │
   │   TCP    │
   ▼          │
 ┌─┴──────────┴─┐
 │  C++ Server  │
 │ (eop-server) │
 └──────┬───────┘
        │
        │ OTLP/HTTP
        ▼
 ┌──────────────┐
 │    OTel      │
 │  Collector   │
 └──────┬───────┘
        │
        ▼
 ┌──────────────┐
 │    SigNoz    │
 │ (Traces UI)  │
 └──────────────┘
 ```


| Component | Language | Description |
|-----------|----------|-------------|
| eop-server | C++17 | TCP server with thread pool, node registry, heartbeat monitor |
| libeop_client | C99 | Client library with opaque handle pattern |
| Observability | OTel + SigNoz; Prometheus + Grafana | Traces and logs with trace_id; SLO dashboards (US-205) |
| Deployment | Docker + K8s | Multi-stage build (distroless, ≤ 50 MB), liveness/readiness probes |

## Team

| Name | GitHub | Role |
|------|--------|------|
| Costamagna, Matias Javier | @Mati-Costamagna | Eng-B — Client lib, protocol, build system |
| Bejarano, Kevin Matias Nicolas | @KevinGTH | Eng-A — Server core, concurrency, registry |
| Miglierini, Carlos Emanuel | @miglierinicarlos | Eng-C — Deployment, observability, infra |

## Quick Start

### Docker Compose (recommended)

Launches the full stack: eop-server, OTel Collector, SigNoz, ClickHouse, ZooKeeper.

```bash
make deploy-local
```

The server accepts TCP connections on **localhost:9026**.
Prometheus text metrics for the C++ server: **http://localhost:9464/metrics** (port from `EOP_METRICS_PORT`, default 9464; set to `0` to disable HTTP exposition).
SigNoz UI is available at http://localhost:8080.

### Prometheus and Grafana (US-205)

After `make deploy-local` or `docker compose up -d`:

| UI | URL | Notes |
|----|-----|--------|
| Grafana | http://localhost:3000 | Default user/password: `admin` / `admin` (override with `GRAFANA_ADMIN_USER` / `GRAFANA_ADMIN_PASSWORD`). Change port with `GRAFANA_PORT`. |
| Prometheus | http://localhost:9090 | Targets, rules, and firing alerts: **Status → Targets** and **Alerts**. Change port with `PROMETHEUS_PORT`. |

Dashboard **EOP SLO Overview** is loaded automatically from `monitoring/grafana/dashboards/eop-slo-overview.json` (no manual import).

**How to read the panels (scaffold):**

1. **Uptime % (24h, TCP probe)** — Successful TCP connects (blackbox) to the EOP TCP port over 24h. Complements `/metrics`: TCP can be up while the process misbehaves.
2. **Error budget remaining (proxy)** — Linear proxy vs **99.9%** using the TCP probe; tune when you base the budget on `eop_messages_total` success ratio (ADR-009).
3. **Requests/s** — `rate(eop_messages_total{job="eop-server"}[5m])` by **operation** label (`REGISTER`, `QUERY_NODE`, `LIST_NODES`, `HEARTBEAT`, `UNKNOWN`).
4. **P99 latency** — P99 of **`eop_message_duration_seconds`** (handler time after a valid frame read), by **operation**.

Prometheus scrapes every **15s** (global `scrape_interval`). Alert **EOPErrorBudgetProxyBelow50** fires when the proxy budget stays under 50% for 2 minutes; see **Prometheus → Alerts** (wire Alertmanager or Grafana contact points later for email/Slack/log shipping).

```bash
make logs           # stream server logs
make down           # tear down the stack
make check-size     # verify runtime image ≤ 50 MB (ADR-006)
```

### Kubernetes

Requires a running cluster and the eop-server image built locally or in a registry.

```bash
# Build the image
docker build -t eop-server:latest .

# Deploy
kubectl apply -f k8s/

# Verify
kubectl get pods -l app=eop-server
```

Manifests in k8s/ include: Deployment with liveness/readiness probes,
Service, and ConfigMap for environment-based configuration.

```bash
make validate-k8s               # lint manifests with kubeconform
make test-k8s-recovery          # end-to-end pod restart + client reconnect
make test-k8s-autorestart       # kill PID 1, verify automatic restart
```

## Build from Source

### Prerequisites

- CMake ≥ 3.28
- C++17 compiler (GCC 12+ or Clang 15+)
- [vcpkg](https://github.com/microsoft/vcpkg) (dependencies: nlohmann_json, opentelemetry-cpp, GTest)

### Compile

```bash
cmake -S . -B build \
-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
-DCMAKE_BUILD_TYPE=Release

cmake --build build -j$(nproc)
```

The build produces:
- build/exodus_app — the eop-server binary
- build/client/libeop_client.so / libeop_client.a — shared and static client library

### Run

```bash
# Server (reads config from environment)
EOP_PORT=9026 EOP_THREAD_POOL_SIZE=16 EOP_MAX_CLIENTS=10000 \
EOP_HEARTBEAT_INTERVAL=5 EOP_IDLE_TIMEOUT=30 \
./build/exodus_app
```

## Configuration

All configuration is externalized through environment variables (no hardcoded values).

| Variable | Default | Description                            |
|----------|---------|----------------------------------------|
| EOP_PORT | 9026 | TCP listen port                        |
| EOP_THREAD_POOL_SIZE | 16 | Worker threads for connection handling |
| EOP_MAX_CLIENTS | 10000 | Maximum concurrent TCP clients         |
| EOP_IDLE_TIMEOUT | 30 | Seconds before idle connection cleanup |
| EOP_HEARTBEAT_INTERVAL | 5 | Seconds between heartbeat checks       |
| OTEL_EXPORTER_OTLP_ENDPOINT | — | OTel Collector HTTP endpoint           |
| OTEL_SERVICE_NAME | — | Service name for traces                |

## Client Library

The C99 client library (libeop_client) provides an opaque handle API:

```c
#include "eop_client.h"

// Connect (5000 ms timeout)
eop_client_t* client = eop_connect("127.0.0.1", 9026, 5000);

// Send a command and receive response
const char* payload = "{\"node_id\":\"vault-13\"}";
eop_response_t* resp = eop_send_command(
client, EOP_REGISTER,
(const uint8_t*)payload, strlen(payload));

// Inspect response
if (resp && resp->msg_type == EOP_ACK) {
printf("Registered successfully\n");
}

// Cleanup
eop_response_free(resp);
eop_disconnect(client);
client = NULL;
```

Reconnection after pod restart:

```c
eop_error_code rc = eop_reconnect(client, 3000);
if (rc == EOP_OK) {
// Re-register node on the new connection
resp = eop_send_command(client, EOP_REGISTER,
(const uint8_t*)payload, strlen(payload));
eop_response_free(resp);
}
```

## Tests

EOP uses a comprehensive test suite across unit, integration, and sanitizer builds
to ensure code quality, memory safety, and thread safety (NFR-3 compliance).

### Unit Tests

Run the full unit test suite (covers server components, client library, handlers):

```bash
cmake --build build -j$(nproc) --target unit_tests
ctest --test-dir build --output-on-failure
```

All unit tests run in CI on every PR and must pass before merge.

### Memory Safety (AddressSanitizer)

Detects memory leaks, use-after-free, buffer overflows. Required for all C/C++
code (NFR-3: "Zero memory leaks").

```bash
cmake -S . -B build-asan \
-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
-DCMAKE_BUILD_TYPE=Debug \
-DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
-DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer"

cmake --build build-asan -j$(nproc)
./build-asan/tests/unit_tests
./build-asan/tests/eop_client_disconnect_test  # Client library stress test
```

### Data Race Detection (ThreadSanitizer)

Detects concurrent data races under multi-threaded workloads. Verifies thread
safety of NodeRegistry, ThreadPool, WorkQueue (NFR-3: "Zero data races").

```bash
cmake -S . -B build-tsan \
-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
-DCMAKE_BUILD_TYPE=Debug \
-DCMAKE_CXX_FLAGS="-fsanitize=thread" \
-DCMAKE_C_FLAGS="-fsanitize=thread"

cmake --build build-tsan -j$(nproc)
./build-tsan/tests/unit_tests
```

### Integration Tests

End-to-end tests that exercise the full server: client connections, protocol
handshake, registration, queries, heartbeats, disconnection. Requires a running
server.

```bash
# Terminal 1: Start the server
./build/exodus_app &
SERVER_PID=$!

# Terminal 2: Run integration tests
./build/tests/integration_tests --gtest_output=xml

# Cleanup
kill $SERVER_PID
```

Key integration test suites:
- *Client lifecycle*: connect → register → query → disconnect
- *Pod restart recovery* (K8s): pod crash → client reconnects → re-registers
- *Concurrent stress*: 10 threads × 100 cycles, zero memory leaks/races

### Coverage

Coverage gates are enforced in CI:

```bash
cmake -S . -B build \
-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
-DCMAKE_BUILD_TYPE=Debug \
-DEXODUS_ENABLE_COVERAGE=ON

cmake --build build -j$(nproc)
ctest --test-dir build
lcov --capture --directory build --output-file coverage.info
genhtml coverage.info --output-directory coverage_html
```

Target: ≥ 90% coverage on business logic (NodeRegistry, handlers, protocol serialization).

## Observability

EOP is fully instrumented with OpenTelemetry (OTel) and integrated with SigNoz
for distributed tracing. All operations emit spans, and logs include trace_id
for correlation (US-110, ADR-005).

### Quick Access

After make deploy-local:

- *SigNoz UI*: http://localhost:8080
- *Traces available within*: 30 seconds of operation

### Instrumented Operations

The following server operations emit OTel spans:

| Operation | Span Name | Details |
|-----------|-----------|---------|
| Node registration | register_node | Includes node_id, registration timestamp |
| Node query | query_node | Includes node_id, status |
| Node list | list_nodes | Includes count of registered nodes |
| Heartbeat check | heartbeat_handler | Includes stale node detection |
| Client connection | connection_worker | Includes session_id, peer IP |
| Disconnect detection | node_offline | Includes reason (timeout/clean FIN) |

### Log ↔ Trace Correlation

All structured JSON logs include trace_id and span_id fields, enabling direct
correlation with SigNoz traces:

```json
{
"timestamp": "2026-04-12T10:30:45.123Z",
"level": "INFO",
"service": "eop-server",
"trace_id": "a1b2c3d4e5f6g7h8",
"span_id": "i9j0k1l2m3n4o5p6",
"message": "Node vault-13 registered successfully",
"node_id": "vault-13",
"session_id": 42
}
```


In SigNoz, click on any trace to see all logs (server-side and client-side)
associated with that trace_id.

### Example: Tracing a Registration Flow

1. *Client side* (your code):
   ```c
   eop_client_t* client = eop_connect("localhost", 9026, 5000);
   eop_response_t* resp = eop_send_command(client, EOP_REGISTER, ...);
   ```


2. *Server side* (auto-instrumented):
   - Span connection_worker created (session start)
   - Span register_node created (inside ConnectionWorker)
   - OTel Collector forwards spans to SigNoz

3. *Observe in SigNoz*:
   - Open http://localhost:8080
   - Go to *Traces*
   - Search by service name: eop-server
   - Click on a register_node span to see the full flow with timing and logs

### Runbook: Navigate from a node_id to its log lines

Use this workflow when you know a `node_id` and want to find all log lines
associated with its operations.

1. **Find the trace in SigNoz**
   - Open http://localhost:8080 → go to *Traces*
   - In the filter bar, add attribute: `node.id = <node_id>` (e.g. `node.id = vault-13`)
   - Select any span from the result list and copy its `traceId` value

2. **Navigate to the correlated logs**
   - Go to *Logs* in the SigNoz sidebar
   - Add filter: `trace_id = <traceId copied above>`
   - All structured JSON log lines emitted during that operation will appear,
     each containing `node_id`, `event`, `session_id`, and `span_id`

3. **Cross-reference multiple operations for the same node**
   - Repeat step 1 with the same `node.id` filter but no time restriction
   - Each trace corresponds to one operation (REGISTER, QUERY_NODE, etc.)
   - The `trace_id` in every log line links it back to its span in SigNoz

### Configuration

OTel export is configured via environment variables:

```bash
OTEL_EXPORTER_OTLP_ENDPOINT=http://otel-collector:4318
OTEL_SERVICE_NAME=eop-server
```


In docker-compose, the OTel Collector listens on port 4318 (HTTP) and forwards
spans to SigNoz's ClickHouse backend. The entire stack launches with:

```bash
make deploy-local
```


### Troubleshooting Traces

*Traces not appearing in SigNoz?*

1. Verify the OTel Collector is running:
   ```bash
   docker ps | grep otel-collector
   docker logs signoz-otel-collector
   ```


2. Check server logs for export errors:
   ```bash
   make logs
   ```


3. Verify Collector is connected to SigNoz:
   ```bash
   docker logs signoz
   ```


For full observability details, see [ADR-005](docs/adr/ADR-005-observability-strategy.md).

## Protocol

Wire format: length-prefix framing (4 bytes big-endian) + 10-byte envelope + JSON payload.
See [ADR-003](docs/adr/ADR-003-message-protocol.md) for the full specification.

| Type | ID | Direction |
|------|----|-----------|
| REGISTER | 0x01 | Client → Server |
| ACK | 0x02 | Server → Client |
| ERROR | 0x03 | Server → Client |
| QUERY_NODE | 0x04 | Client → Server |
| LIST_NODES | 0x05 | Client → Server |
| HEARTBEAT | 0x06 | Client → Server |

## Documentation

| Document | Description |
|----------|-------------|
| [CHANGELOG.md](CHANGELOG.md) | Release history |
| [docs/adr/](docs/adr/) | Architectural Decision Records (ADR-001 through ADR-015) |

### ADR Index

| ADR | Decision |
|-----|----------|
| [ADR-001](docs/adr/ADR-001-ipc-mechanism.md) | BSD/POSIX sockets, no RPC frameworks |
| [ADR-002](docs/adr/ADR-002-concurrency-model.md) | Thread pool concurrency model |
| [ADR-003](docs/adr/ADR-003-message-protocol.md) | Length-prefix framing, JSON encoding |
| [ADR-004](docs/adr/ADR-004-service-boundaries.md) | Single service boundary for v0.1 |
| [ADR-005](docs/adr/ADR-005-observability-strategy.md) | OTel + SigNoz, structured JSON logs |
| [ADR-006](docs/adr/ADR-006-containerization-k8s.md) | Docker multi-stage + K8s deployment |
| [ADR-015](docs/adr/ADR-015-client-reconnection-policy.md) | Client reconnection with exponential backoff |

## License

This repository is part of an academic engineering program (Sistemas Distribuidos — Ingeniería en Computación, Universidad Nacional de Córdoba) and is intended exclusively for educational purposes
