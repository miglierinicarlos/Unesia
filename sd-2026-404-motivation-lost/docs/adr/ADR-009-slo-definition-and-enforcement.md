# ADR-009: SLO Definition and Enforcement

| Field | Value |
|-------|-------|
| **Date** | Week 6 / 2026-04-07 |
| **Status** | Accepted |
| **Lab** | v0.2 |
| **Deciders** | Eng-A, Eng-B, Eng-C |

## Context

EOP must meet an availability commitment of ≥ 99.9% monthly with the Vault-Tec Reconstruction Division (NFR-2). Without a mechanism to measure and alert on this commitment in real time, there is no way to know whether it is being met until a client reports an outage.

ADR-005 established that Prometheus + Grafana are the metrics and SLO layer, introduced in v0.2. This ADR defines what is measured, how SLOs are computed from those measurements, what constitutes a policy violation, and how dashboards and alerts are structured as versioned repository artifacts.

The v0.1 server and HPC engine are already instrumented with OpenTelemetry spans (ADR-005). Prometheus provides a complementary signal: aggregated counters and histograms over time, enabling burn-rate computation and budget tracking that distributed traces alone cannot provide.

## Decision

We will implement SLOs as code: every SLO target, PromQL expression, alert threshold, and dashboard panel definition is committed to the repository under `monitoring/`. No SLO configuration exists only in a running Grafana or Prometheus instance.

### SLO Definitions

| SLO | Target | Measurement Window |
|-----|--------|--------------------|
| Availability | ≥ 99.9% | Rolling 30 days |
| P99 request latency (eop-server) | < 200 ms | Rolling 1 hour |
| P99 REST latency (API gateway, from v0.3) | < 300 ms | Rolling 1 hour |

Availability is defined as the ratio of successful requests to total requests:

```
availability = 1 - (rate(eop_requests_total{result="error"}[30d]) / rate(eop_requests_total[30d]))
```

An error is any response with an EOP ERROR message type or a connection reset before a response is sent.

### Error Budget Policy

The error budget for 99.9% availability over 30 days is **43.2 minutes** of allowed downtime. When more than 50% of the budget is consumed, an alert fires and no new feature work is merged until the budget recovers above 75%.

### Metrics Exposed

Each EOP service exposes a `/metrics` endpoint in Prometheus exposition format:

| Metric | Type | Labels | Purpose |
|--------|------|--------|---------|
| `eop_requests_total` | Counter | `service`, `message_type`, `result` | Availability and RPS computation |
| `eop_request_duration_seconds` | Histogram | `service`, `message_type` | P99 latency computation |
| `eop_active_connections` | Gauge | `service` | Connection saturation monitoring |
| `eop_hpc_analysis_duration_seconds` | Histogram | `algorithm` | HPC engine performance per algorithm |
| `eop_hpc_speedup_ratio` | Gauge | `threads`, `graph_size` | Parallel speedup tracking |

### Repository Structure

```
monitoring/
├── prometheus/
│   ├── prometheus.yml          # Scrape config: K8s pod annotations, 15 s interval
│   ├── alerts.yaml             # Alert rules: error budget < 50%, P99 breach
│   └── recording-rules.yaml    # Pre-computed rates for dashboard performance
└── grafana/
    ├── dashboards/
    │   ├── slo-dashboard.json          # Availability gauge, error budget, P99, RPS
    │   └── hpc-performance.json        # Speedup ratio, algorithm duration, thread utilization
    └── provisioning/
        ├── dashboards/dashboards.yaml  # Grafana dashboard provisioning config
        └── datasources/prometheus.yaml # Prometheus datasource definition
```

All files in `monitoring/` are provisioned automatically on stack startup. Manual dashboard import or alert configuration in the Grafana UI is prohibited — changes must go through a PR.

Prometheus scrapes all EOP pods via K8s pod annotations (`prometheus.io/scrape: "true"`, `prometheus.io/port`). Scrape interval is 15 seconds for all services.

## Alternatives Considered

| Alternative | Reason Rejected |
|-------------|----------------|
| Define SLOs only in Grafana UI | Dashboard JSON lives only in the running instance. It is lost on pod restart unless manually backed up. Not reproducible from a fresh cluster apply. Violates the "full stack from a single command" requirement (NFR-4). |
| Use SigNoz for SLO dashboards | SigNoz already handles distributed traces (ADR-005). Adding metrics and SLO dashboards to SigNoz would mix the trace-causality tool with the time-series aggregation tool. Prometheus + Grafana is the standard for this layer. |
| Define SLOs as uptime checks (blackbox probing) | Uptime checks measure reachability, not request success rate. They cannot distinguish between a crashed pod and a pod that returns errors for 30% of requests. The EOP SLO is defined on error rate, not reachability. |
| Use only OTel metrics pipeline (no Prometheus) | The OTel Collector can export metrics to Prometheus-compatible backends, but introduces an additional translation layer. Exposing `/metrics` directly from each service is simpler and removes the OTel Collector as a single point of failure for the metrics path. |

## Consequences

**Positive:**
- SLO targets, alert thresholds, and dashboard definitions are reviewed in PRs and tracked in git history. Any change to an SLO threshold is a deliberate, reviewed decision.
- The full observability stack deploys from `make deploy-local` or `kubectl apply`. No manual Grafana configuration is needed.
- Prometheus recording rules pre-compute expensive PromQL expressions, keeping dashboard load times under 2 seconds even over 30-day windows.
- The error budget policy creates a direct link between reliability incidents and feature velocity, making reliability trade-offs explicit.

**Negative:**
- Each EOP service must expose a `/metrics` HTTP endpoint. This adds a second listener port per service (distinct from the EOP TCP protocol port), which must be declared in K8s manifests and ConfigMaps.
- Dashboard JSON files are verbose and difficult to review in diffs. Changes made in the Grafana UI must be exported and committed manually — the export step is easy to forget.
- The 15-second scrape interval means the minimum alerting latency is 15 seconds plus the Prometheus evaluation interval (also 15 seconds by default). Sub-30-second incident detection requires reducing these intervals, which increases Prometheus resource consumption.

**Constraints Introduced:**
- Every EOP service added from v0.2 onward must expose `eop_requests_total` and `eop_request_duration_seconds` with the label schema defined above. Omitting these metrics from a new service is an AC violation that blocks merge.
- No SLO threshold, alert expression, or dashboard panel may exist only in a running Grafana or Prometheus instance. All definitions live in `monitoring/` and are provisioned automatically. Manual UI configuration is prohibited.
- The Prometheus scrape port for each service must be declared in the K8s Deployment manifest as `prometheus.io/port` annotation and in the corresponding ConfigMap. Hardcoding the port in source is prohibited (NFR-4: all configuration externalized).
- The SLO dashboard must use the repository-defined PromQL expressions. Diverging from `eop_requests_total{result="error"}` for availability computation requires an update to this ADR.
- Alert rules in `monitoring/prometheus/alerts.yaml` are the authoritative definition of firing conditions. Alerts configured only in Grafana (without a corresponding Prometheus rule) are not valid and will be removed on the next stack redeploy.
