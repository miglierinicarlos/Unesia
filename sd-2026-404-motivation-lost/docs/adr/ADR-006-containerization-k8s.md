# ADR-006: Containerization and Kubernetes Adoption

| Field | Value |
|-------|-------|
| **Date** | Week 1 |
| **Status** | Accepted |
| **Lab** | v0.1 |
| **Deciders** | All squads + Instructor |

## Context

EOP is delivered to Vault-Tec as a product, not as source code. The client must be able to deploy the system reproducibly on their infrastructure. Vault-Tec operates a Kubernetes cluster. The deployment must be manageable by their operations team without deep knowledge of the EOP codebase.

## Decision

Containerization is mandatory from v0.1. Kubernetes is the deployment target from v0.1.

### Docker — Build Requirements

- **Multi-stage Dockerfiles** are required for all C++ services. Build stage contains the compiler toolchain. Runtime stage contains only the compiled binary and its runtime dependencies
- **Runtime image ≤ 50 MB.** Base image choices: `debian:bookworm-slim`, `alpine:3.19`, or `gcr.io/distroless/cc-debian12`
- **ENTRYPOINT must use JSON array form:** `["./eop-server"]`. Shell form causes the shell to be PID 1, which does not propagate SIGTERM to the application
- **Graceful shutdown on SIGTERM:** stop accepting new connections, allow active connections to complete (maximum 15 s), exit with code 0

### Kubernetes — Manifest Requirements

All services must have:
- A Deployment with `replicas: 1` minimum (configurable via ConfigMap)
- A **liveness probe** (TCP socket check or HTTP GET to `/health`) with `initialDelaySeconds: 5`, `periodSeconds: 10`, `failureThreshold: 3`
- A **readiness probe** (same mechanism as liveness). Pod is not added to Service endpoints until readiness passes
- `resources.requests` and `resources.limits` defined for both CPU and memory
- A **ConfigMap** for all service configuration. No environment-specific values hardcoded in the image

### Local Development

- `docker-compose.yml` provides the complete local development stack: all EOP services + OTel Collector + SigNoz
- `docker compose up` must bring the full stack to a healthy state in under 60 seconds

## Alternatives Considered

| Alternative | Reason Rejected |
|-------------|----------------|
| Docker Compose only (no K8s) | Does not provide pod lifecycle management, health-based restarts, or deployment strategies required in v0.3 and v1.0 |
| Introduce K8s in v0.2 | Canary release (v0.3) and edge ingestion (v1.0) build on K8s primitives. Starting late compresses the learning curve. |
| Virtual machines | Higher overhead, slower iteration, not aligned with modern distributed systems practices |

## Consequences

- The K8s cluster for local development is minikube or k3s (documented in README)
- All K8s manifests are validated by `kubeconform` in CI. Invalid manifests block merge
- CI pushes tagged images on every merge to main
- The image registry is documented in the README
