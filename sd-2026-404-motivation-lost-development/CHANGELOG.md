# Changelog

All notable changes to the Exodus Ops Platform will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/)
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] — 2026-04-12

Core Communication Layer — C++17 TCP server with POSIX sockets, C99 client
library, Kubernetes deployment with automatic recovery, and distributed tracing
in SigNoz.

### Added

#### E1 — C++ TCP Server
- ServerConfig with environment variable parsing and range validation (#30)
- SocketAcceptor with RAII fd guard and accept loop (#30)
- SessionManager with atomic counter-based ID generation (#30)
- MessageSerializer implementing ADR-003 length-prefix framing with JSON
  payloads (#30)
- Thread-safe WorkQueue for connection handoff to configurable ThreadPool;
  EOP_MAX_CLIENTS with ERR_CAPACITY response (#32)
- Thread-safe NodeRegistry with REGISTER, QUERY_NODE and LIST_NODES dispatch;
  RegisterHandler with duplicate detection; QueryHandler (#33)
- HeartbeatHandler for timestamp tracking, HeartbeatMonitor background
  thread with stale node detection and configurable missed-beat tolerance (#34)
- Server-side disconnect detection and node state update to OFFLINE (#34)
- Crash reason logging on fatal signals for pod diagnostics (#43)
- Graceful shutdown on SIGTERM with 15 s hard timeout — ADR-006 (#43)

#### E2 — C Client Library
- Opaque handle pattern with eop_connect, eop_last_error, shared (.so)
  and static (.a) CMake targets (#10)
- eop_send_command with blocking send and partial recv, envelope
  serialization, eop_response_free lifecycle management (#11)
- Configurable connect timeout via timeout_ms parameter (#88)
- Clean disconnection with shutdown(SHUT_RDWR) and full resource release (#12)
- eop_reconnect with exponential backoff — ADR-015 (#43)
- Sequential stress test (1 000 cycles) and concurrent stress test
  (10 threads × 100 cycles) with ASan/TSan validation (#12)

#### E3 — Containerization & Kubernetes
- Multi-stage Dockerfile with distroless runtime image ≤ 50 MB (#31, #79)
- docker-compose.yml launching full stack: server, OTel Collector, SigNoz,
  ClickHouse, ZooKeeper (#31)
- Kubernetes Deployment and Service manifests with ConfigMap for env-based
  configuration (#56, #58)
- Liveness and readiness probes for automatic pod crash detection (#43)
- eop_health binary for docker-compose healthcheck (#43)
- Pod crash-to-ready recovery time validated < 10 s (#43)
- kubeconform validation job in CI pipeline (#43)

#### E4 — Basic Observability
- OpenTelemetry SDK integrated into server with OTLP exporter to OTel
  Collector (#110)
- OTel spans on register_node, query_node, list_nodes, heartbeat and
  connection worker operations — ADR-005 (#33, #110)
- trace_id and span_id propagation in structured JSON logs (#110)
- OTel Collector configured to forward spans to SigNoz (#110)
- Traces visible in SigNoz UI within 30 s of emission (#110)
- Integration test validating end-to-end trace generation (#110)

#### Build & CI
- Repository scaffold with project structure and issue templates (#1, #3)
- CI pipeline: docker build job, clang-format, clang-tidy, ASan, TSan,
  coverage flags on C/C++ targets (#1, #31)
- vcpkg GHA binary cache wired into Docker build for deterministic
  dependencies (#43)

### Fixed
- Client library timeout and shutdown reliability issues (#88)
- CI branch validation extended to fix/ prefix (#88)
- vcpkg binary cache migrated from deprecated x-gha backend to files
  provider (#88)
- K8s service targetPort aligned to named port eop-tcp (#43)

### Documentation
- ADR-001: BSD/POSIX sockets, no RPC frameworks
- ADR-002: Thread pool concurrency model
- ADR-003: Length-prefix framing, JSON encoding, versioning policy
- ADR-004: Single service boundary for v0.1
- ADR-005: Observability strategy — OTel SDK + structured JSON logs
- ADR-006: Containerization & Kubernetes adoption
- ADR-015: Explicit client reconnection policy with retry parameters and
  thread-safety notes (#43)

[Unreleased]: https://github.com/ICOMP-UNC/sd-2026-404-motivation-lost/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/ICOMP-UNC/sd-2026-404-motivation-lost/releases/tag/v0.1.0
