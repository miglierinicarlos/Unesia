# ADR-003 Addendum: ANALYZE_GRAPH / ANALYZE_RESULT Protocol Extension

| Field | Value |
|-------|-------|
| **Date** | 17-04-2026 |
| **Status** | Accepted |
| **Lab** | v0.2 |
| **Deciders** | Squad 404-motivation-lost |
| **Extends** | ADR-003 (Message Protocol Design) |

## Context

v0.2 introduces an HPC engine that performs parallel graph analysis (Dijkstra, connected components, centrality) using OpenMP (ADR-007). The engine connects to the EOP server as a standard EOP node (ADR-008), registering itself via the existing `REGISTER` (0x01) message type.

Clients (e.g., the Go API gateway in v0.3, integration tests, or any EOP-compatible client) need a way to trigger graph analysis and receive results without knowing the HPC engine's node ID, IP address, or internal routing. The v0.1 server acts as the routing intermediary.

Two new message type IDs — `0x07` and `0x08` — were reserved in ADR-003 for this purpose. This addendum defines their payload schemas, routing semantics, error conditions, and backward-compatibility contract.

This document is the protocol contract for the ANALYZE_GRAPH / ANALYZE_RESULT interaction. An independent implementer must be able to build a compatible client from this addendum alone, without reading server source code.

## Decision

We will add two new message types to the ADR-003 envelope: `ANALYZE_GRAPH` (0x07) for client-to-server analysis requests, and `ANALYZE_RESULT` (0x08) for server-to-client results. Routing from the v0.1 server to the HPC engine, and the result path back, are handled transparently by the server. The `trace_id` field propagates OTel context across the hop.

This extension is non-breaking per the ADR-003 versioning policy: new message type IDs that did not exist in v0.1 are added, no existing fields are modified, and `protocol_version` remains `1`.

### New Message Types

The following rows extend the message type table in ADR-003:

| ID | Type | Direction | Since | Payload |
|----|------|-----------|-------|---------|
| `0x07` | `ANALYZE_GRAPH` | Client → Server | v0.2 | Graph data + algorithm list |
| `0x08` | `ANALYZE_RESULT` | Server → Client | v0.2 | Per-algorithm analysis results |

### Payload Schemas

#### ANALYZE_GRAPH (0x07) — Client → Server

The client sends graph topology and the set of algorithms to run. The server forwards the request to the registered HPC engine and waits for a result.

```json
{
  "algorithm": ["dijkstra", "connected_components", "centrality"],
  "graph": {
    "node_count": 4,
    "edge_count": 4,
    "edges": [
      {"src": 0, "dst": 1, "weight": 1.0},
      {"src": 1, "dst": 2, "weight": 2.5},
      {"src": 2, "dst": 3, "weight": 1.5},
      {"src": 0, "dst": 3, "weight": 4.0}
    ]
  },
  "source_node": 0,
  "trace_id": "4bf92f3577b34da6a3ce929d0e0e4736"
}
```

**Field reference:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `algorithm` | array of string | Yes | One or more of: `"dijkstra"`, `"connected_components"`, `"centrality"`. Unknown values are ignored by the engine; the server does not validate them. |
| `graph` | object | Yes | Graph topology. See sub-fields below. |
| `graph.node_count` | uint32 | Yes | Number of nodes. Nodes are identified by zero-based integer indices `[0, node_count)`. |
| `graph.edge_count` | uint32 | Yes | Number of edges in the `edges` array. Must equal `edges.length`. |
| `graph.edges` | array of object | Yes | Adjacency list. Each entry has `src` (uint32), `dst` (uint32), and `weight` (float64). |
| `graph.edges[].src` | uint32 | Yes | Source node index. Must be in `[0, node_count)`. |
| `graph.edges[].dst` | uint32 | Yes | Destination node index. Must be in `[0, node_count)`. |
| `graph.edges[].weight` | float64 | Yes | Edge weight. Must be finite and non-negative. |
| `source_node` | uint32 | No | Source node for Dijkstra. Required when `"dijkstra"` is in `algorithm`; ignored otherwise. Must be in `[0, node_count)`. |
| `trace_id` | string | No | OTel W3C trace ID (32 hex chars). When present, the server and HPC engine attach this as the parent span context. Recommended for all production calls. |

**Validation rules enforced by the server before forwarding:**
- `algorithm` must be present and non-empty. If absent or empty: `ERR_MALFORMED` (0x03).
- `graph` must be present with `node_count ≥ 1`, `edge_count ≥ 0`, and `edges` present. If malformed: `ERR_MALFORMED` (0x03).
- Node indices in `edges` must be `< node_count`. If out of range: `ERR_MALFORMED` (0x03).
- Receivers MUST ignore unknown fields (forward compatibility per ADR-003).

#### ANALYZE_RESULT (0x08) — Server → Client

The server sends results after the HPC engine completes processing. The `ref_message_id` matches the `message_id` of the originating `ANALYZE_GRAPH` request.

```json
{
  "ref_message_id": 12348,
  "dijkstra": {
    "distances": [0.0, 1.0, 3.5, 4.0],
    "source_node": 0
  },
  "connected_components": {
    "component_count": 1,
    "components": [[0, 1, 2, 3]]
  },
  "centrality": {
    "scores": [0.75, 0.50, 0.50, 0.25]
  },
  "trace_id": "4bf92f3577b34da6a3ce929d0e0e4736"
}
```

**Field reference:**

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `ref_message_id` | uint32 | Yes | Mirrors the `message_id` from the `ANALYZE_GRAPH` envelope. Used by the client to correlate the result with the request. |
| `dijkstra` | object | If requested | Present only when `"dijkstra"` was in the request's `algorithm` list. |
| `dijkstra.distances` | array of float64 | Yes (if present) | Shortest-path distance from `source_node` to each node. Index corresponds to node index. Unreachable nodes have value `+Inf` (JSON: `null`). |
| `dijkstra.source_node` | uint32 | Yes (if present) | Echoes the source node from the request. |
| `connected_components` | object | If requested | Present only when `"connected_components"` was in the request's `algorithm` list. |
| `connected_components.component_count` | uint32 | Yes (if present) | Number of distinct connected components found. |
| `connected_components.components` | array of array of uint32 | Yes (if present) | Each inner array lists the node indices belonging to one component. |
| `centrality` | object | If requested | Present only when `"centrality"` was in the request's `algorithm` list. |
| `centrality.scores` | array of float64 | Yes (if present) | Betweenness or degree centrality score per node. Index corresponds to node index. |
| `trace_id` | string | No | Echoed from the request if present. Allows the client to close its OTel span. |

Result fields for algorithms not requested in the originating `ANALYZE_GRAPH` MUST NOT be present in the response. Clients MUST ignore unexpected fields.

### Routing Semantics

1. The client sends `ANALYZE_GRAPH` (0x07) to the v0.1 server over its established TCP connection.
2. The server's `ConnectionWorker` dispatches the message to `AnalyzeHandler`.
3. `AnalyzeHandler` looks up the registered HPC engine node (registered via `REGISTER` with a known `node_id`, e.g., `"hpc-engine-1"`).
4. The server forwards the request payload to the HPC engine over the engine's existing TCP connection using the same envelope format.
5. The HPC engine processes the graph and responds with `ANALYZE_RESULT` (0x08) on its own connection.
6. The server receives the result and forwards it to the originating client connection.
7. The `ref_message_id` in the result matches the client's original `message_id`, enabling correlation.

The client is unaware of the HPC engine's existence, address, or node ID. The server is the sole routing layer.

**Timeout:** The server waits at most `EOP_ANALYZE_TIMEOUT_MS` milliseconds for the HPC engine to respond. If the deadline is exceeded, the server sends `ERROR` (0x03) with `error_code = 0x05` (`ERR_TIMEOUT`) to the client.

### Error Conditions

| Error Code | Name | When |
|------------|------|------|
| `0x03` | `ERR_MALFORMED` | `ANALYZE_GRAPH` payload missing required fields, empty `algorithm` array, out-of-range node index, or non-finite edge weight |
| `0x04` | `ERR_SERVICE_UNAVAILABLE` | No HPC engine is currently registered or the engine's connection is closed |
| `0x05` | `ERR_TIMEOUT` | The HPC engine did not respond within `EOP_ANALYZE_TIMEOUT_MS` |
| `0xFF` | `ERR_INTERNAL` | Unexpected internal failure during routing or result forwarding |

All error responses use the standard `ERROR` (0x03) envelope with `ref_message_id` matching the client's `ANALYZE_GRAPH` request.

### Trace Context Propagation

When `trace_id` is present in the `ANALYZE_GRAPH` payload, the server MUST include it in the forwarded payload to the HPC engine. The HPC engine MUST use it as the parent span context when emitting OTel spans. The server MUST echo it in the `ANALYZE_RESULT` payload returned to the client. This enables end-to-end trace linkage: client span → server span → HPC engine span, all visible in SigNoz (ADR-005).

The `trace_id` field is optional to maintain backward compatibility with clients that do not use OTel. Its absence does not affect routing or result correctness.

## Backward Compatibility

Type IDs `0x07` and `0x08` were explicitly reserved in ADR-003 and unused in all v0.1 server code. The extension is non-breaking for two distinct reasons:

1. **Clients that don't send 0x07 are entirely unaffected.** The new types are additive. No v0.1 client behavior changes. A v0.1 client that only sends `REGISTER`, `QUERY_NODE`, `LIST_NODES`, and `HEARTBEAT` sees no change in server behavior.

2. **Safe degradation on a v0.1 server without the v0.2 patch.** ADR-003 mandates that a server receiving an unsupported `message_type` MUST respond with `ERROR` (code `ERR_UNKNOWN_TYPE`, 0x07). A client that sends `ANALYZE_GRAPH` to a non-upgraded server receives a well-formed `ERR_UNKNOWN_TYPE` error — it does not crash, deadlock, or corrupt the stream. This is safe degradation by protocol contract.

3. **`protocol_version` remains `1`.** No envelope fields were modified. The addition of new `message_type` values is a non-breaking change per the ADR-003 versioning policy table.

## Alternatives Considered

| Alternative | Reason Rejected |
|-------------|----------------|
| Client connects directly to HPC engine | Requires clients to know the engine's address. Breaks the single-service boundary (ADR-004). Client reconnection, service discovery, and load balancing become client responsibilities. |
| New TCP port for ANALYZE_GRAPH | Adds a second connection per client, complicating the client library and K8s service definitions. Contradicts ADR-004's single-service boundary for v0.1. |
| Separate `trace_id` envelope field | Would require incrementing `protocol_version` (envelope structure change is breaking per ADR-003). Embedding in the JSON payload is non-breaking and sufficient for OTel linkage. |
| Binary graph encoding in payload | Contradicts the ADR-003 decision to use JSON. The 50 MB JSON cost for a 100K-node graph is acknowledged as acceptable for v0.2 (local network). |

## Consequences

**Positive:**
- Clients can trigger graph analysis through the existing TCP connection without additional setup
- Routing is centralized: the server is the single entry point, consistent with ADR-004
- OTel trace context propagates end-to-end (client → server → HPC engine), satisfying ADR-005 observability requirements
- The addendum follows the ADR-003 versioning policy exactly — no breaking change, no `protocol_version` bump

**Negative:**
- The server becomes a routing proxy: it holds the client connection open while waiting for the HPC engine. Concurrent ANALYZE_GRAPH requests consume worker threads for the duration of the analysis (bounded by `EOP_ANALYZE_TIMEOUT_MS`)
- A single HPC engine registration is assumed. Multi-engine routing or load balancing requires a future ADR

**Constraints Introduced:**
- All services that process `ANALYZE_GRAPH` (server, HPC engine) MUST handle the `trace_id` field when present and propagate it without modification
- The server MUST NOT forward an `ANALYZE_GRAPH` to a disconnected engine — it MUST return `ERR_SERVICE_UNAVAILABLE` immediately
- The HPC engine MUST respond with `ANALYZE_RESULT` (0x08) using the same `message_id` from the forwarded envelope, so the server can route the result to the correct client
- Integration tests MUST validate both the success path and the two error conditions (`ERR_SERVICE_UNAVAILABLE`, `ERR_TIMEOUT`) per Task 7
- `EOP_ANALYZE_TIMEOUT_MS` MUST be externalized as a configuration value (no hardcoded literals, per SW Agreement)
