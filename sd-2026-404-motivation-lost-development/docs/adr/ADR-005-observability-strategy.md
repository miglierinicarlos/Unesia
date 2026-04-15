# ADR-005: Observability Strategy

| Field | Value |
|-------|-------|
| **Date** | Week 1 |
| **Status** | Accepted |
| **Lab** | v0.1 |
| **Deciders** | All squads + Instructor |

## Context

EOP is a distributed system. When something goes wrong, the first question is always: "What happened, where, and when?" Without instrumentation, the only diagnostic tool is `kubectl logs` on individual pods — which does not scale and cannot show causality across service boundaries.

Observability must be designed into the system from the start, not added later. Retrofitting distributed tracing into an untraced codebase is significantly more expensive than instrumenting from day one.

## Decision

The observability stack for EOP is introduced incrementally:

| Layer | Tool | Introduced In | Purpose |
|-------|------|--------------|---------|
| Distributed Traces | OpenTelemetry SDK + SigNoz | v0.1 | Trace causality across service boundaries |
| Metrics & SLOs | Prometheus + Grafana | v0.2 | Measure system health over time |
| Centralized Logs | Loki + Grafana Explore | v0.3 | Full log corpus queryable by trace_id |

### OpenTelemetry (OTel) — Instrumentation Standard

All services use the OpenTelemetry SDK regardless of language (C++ or Go). This ensures:

- A single `trace_id` propagates across all services, enabling end-to-end trace reconstruction
- Switching the backend (e.g., from SigNoz to Jaeger) requires only a configuration change, not a code change
- Instrumentation is consistent: span naming, attribute keys, and error recording follow OTel semantic conventions

### Span Requirements (minimum per service)

Every service must emit a span for each of the following:
- Incoming request handling (server-side span)
- Outgoing request to another service (client-side span)
- Error conditions — spans must set `status = ERROR` and include the error message as an attribute

### Log Requirements

All logs must be structured JSON to stdout:

```json
{
  "timestamp": "2077-01-01T00:00:00.000Z",
  "level": "INFO",
  "service": "eop-server",
  "trace_id": "4bf92f3577b34da6a3ce929d0e0e4736",
  "span_id": "00f067aa0ba902b7",
  "message": "Node registered successfully",
  "node_id": "bunker-42"
}
```

The `trace_id` field must match the OTel trace of the request that generated the log, enabling log-to-trace correlation from v0.1 onward.

## Alternatives Considered

| Alternative | Reason Rejected |
|-------------|----------------|
| Custom logging framework | Does not provide distributed trace correlation. Each service's logs are isolated. |
| Jaeger instead of SigNoz | SigNoz provides a more integrated metrics + traces experience in a single UI. |
| ELK Stack | Significantly higher operational overhead. Loki is simpler while providing the core functionality needed. |
| Add observability in v0.3 | Retrofitting distributed tracing into an already-deployed system is significantly harder. Trace IDs must be present from the first service. |

## Consequences

- OTel instrumentation is a mandatory AC in v0.1 (US-110). It is not optional or deferred
- The OTel Collector runs as a separate container in `docker-compose.yml` from v0.1. It buffers telemetry and exports to SigNoz
- The `trace_id` correlation between SigNoz and Loki (available from v0.3) is only possible because the `trace_id` was embedded in logs from v0.1
- Engineers must not log sensitive data (passwords, tokens, PII)
