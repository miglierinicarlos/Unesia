# ADR-003: Message Protocol Design

| Field | Value |
|-------|-------|
| **Date** | 23-03-2026 |
| **Status** | Accepted |
| **Lab** | v0.1 |
| **Deciders** | Squad 404-motivation-lost |

## Context

The EOP services communicate over TCP using a custom protocol (per ADR-001). The protocol must:

1. Define message boundaries unambiguously (framing)
2. Identify message types and their payloads
3. Evolve without breaking existing clients (versioning strategy)
4. Be documented precisely enough that any engineer can implement a compatible client from this ADR alone

The protocol is the most binding technical decision of the project. Every subsequent release builds on top of it.

## Decision

### Framing: Length-Prefix (4 bytes, big-endian)

Every message on the wire is preceded by a 4-byte unsigned integer (network byte order) indicating the total length of the frame that follows (envelope + payload).

```
┌─────────────────────────┐
│ frame_length (4B, BE)   │  ← How many bytes follow
├─────────────────────────┤
│ envelope (10B fixed)    │  ← Header fields
│ payload  (variable)     │  ← JSON data
└─────────────────────────┘
```

**Why length-prefix over delimiter:**

Delimiter-based framing (e.g., `\n`) fails when the payload contains the delimiter character. In v0.2, ANALYZE_GRAPH transmits JSON graphs with thousands of newlines. Escaping introduces fragility: a single escaping bug desynchronizes the entire TCP stream irrecoverably — the receiver interprets payload data as message boundaries, corrupting all subsequent messages. Length-prefix treats the payload as opaque bytes, eliminating this entire class of vulnerabilities. The cost is two `recv()` calls per message instead of one scan loop, which is negligible.

### Envelope: Fixed 10-byte Header

| Offset | Bytes | Field | Type | Description |
|--------|-------|-------|------|-------------|
| 0 | 1 | `protocol_version` | uint8 | Protocol version. Value: `1` for v0.1 |
| 1 | 1 | `message_type` | uint8 | Operation identifier (enum below) |
| 2 | 4 | `message_id` | uint32 BE | Unique per-connection. For request-response correlation and deduplication (v1.0) |
| 6 | 4 | `payload_length` | uint32 BE | Length of payload in bytes. `0` for messages without payload |

Total frame size: `frame_length = 10 + payload_length`

**Design rationale for each field:**
- `protocol_version`: enables protocol evolution. A server receiving version 2 when it only supports version 1 can respond with ERROR and a description, not crash
- `message_type`: single byte supports 256 types. Sufficient for the entire EOP lifecycle
- `message_id`: critical for concurrent operations. When multiple requests are in-flight, the response's `ref_message_id` tells the client which request it answers. Also used for deduplication in v1.0 edge ingestor (US-404 AC3)
- `payload_length`: enables exact buffer allocation and validates frame integrity

### Payload Encoding: JSON

All payloads are UTF-8 encoded JSON. No BOM. No trailing newline.

**Why JSON over binary or Protobuf:**
- Human-readable: debuggable with `netcat`, `tcpdump`, Wireshark without custom tools
- Parsing overhead (~10 µs per message) is negligible for v0.1 volumes (< 1000 msg/s)
- Every language in the stack has native JSON support (C: cJSON, C++: nlohmann/json, Go: encoding/json)
- ADR-001 explicitly rejected Protobuf. Using Protobuf without gRPC contradicts the spirit of that decision
- No shared `.proto` file dependency between components — reduces coordination overhead

**Library choice:** `nlohmann/json` for C++ (header-only, widely adopted), `cJSON` for the C client library (lightweight, C99-compatible).

### Message Types

| ID | Type | Direction | Since | Payload |
|----|------|-----------|-------|---------|
| `0x01` | `REGISTER` | Client → Server | v0.1 | Node metadata |
| `0x02` | `ACK` | Server → Client | v0.1 | Confirmation + optional result |
| `0x03` | `ERROR` | Server → Client | v0.1 | Error code + description |
| `0x04` | `QUERY_NODE` | Client → Server | v0.1 | Node ID |
| `0x05` | `LIST_NODES` | Client → Server | v0.1 | None (payload_length = 0) |
| `0x06` | `HEARTBEAT` | Client → Server | v0.1 | None (payload_length = 0) |
| `0x07` | `ANALYZE_GRAPH` | Client → Server | v0.2 | Graph + algorithms (reserved) |
| `0x08` | `ANALYZE_RESULT` | Server → Client | v0.2 | Analysis results (reserved) |
| `0x09` | `EDGE_TELEMETRY` | Ingestor → Server | v1.0 | Telemetry data (reserved) |
| `0x10–0xFF` | Reserved | — | Future | — |

Types `0x07–0x09` are reserved for future releases. The server MUST respond with `ERROR` (code `ERR_UNKNOWN_TYPE`) if it receives a message type it does not support. It MUST NOT crash or silently discard the message.

### Error Codes

| Code | Name | When Used |
|------|------|-----------|
| `0x01` | `ERR_DUPLICATE` | REGISTER with existing node_id (US-103 AC3) |
| `0x02` | `ERR_NOT_FOUND` | QUERY_NODE with unknown node_id (US-103 AC6) |
| `0x03` | `ERR_MALFORMED` | Invalid JSON payload, missing required fields (US-201 AC6) |
| `0x04` | `ERR_SERVICE_UNAVAILABLE` | Target service offline, e.g., HPC engine (US-203 AC4) |
| `0x05` | `ERR_TIMEOUT` | Operation exceeded configured timeout |
| `0x06` | `ERR_CAPACITY` | Server at EOP_MAX_CLIENTS limit (US-102 AC7) |
| `0x07` | `ERR_UNKNOWN_TYPE` | Received a message_type the server doesn't support |
| `0xFF` | `ERR_INTERNAL` | Unclassified internal error |

### Payload Schemas

#### REGISTER (0x01) — Client → Server

```json
{
  "node_id": "bunker-42",
  "bunker_name": "Vault 42",
  "ip_address": "10.0.1.42",
  "capacity": 100
}
```

All fields required. `node_id` must be unique across the registry.

#### ACK (0x02) — Server → Client

```json
{
  "ref_message_id": 12345,
  "node_id": "bunker-42"
}
```

`ref_message_id` matches the `message_id` of the request that triggered this ACK. Additional fields may be present depending on the request type.

**ACK for QUERY_NODE:**
```json
{
  "ref_message_id": 12346,
  "node_id": "bunker-42",
  "bunker_name": "Vault 42",
  "status": "ONLINE",
  "ip_address": "10.0.1.42",
  "capacity": 100,
  "last_seen_at": "2077-01-01T00:00:00.000Z"
}
```

**ACK for LIST_NODES:**
```json
{
  "ref_message_id": 12347,
  "nodes": [
    {
      "node_id": "bunker-42",
      "bunker_name": "Vault 42",
      "status": "ONLINE",
      "ip_address": "10.0.1.42",
      "capacity": 100,
      "last_seen_at": "2077-01-01T00:00:00.000Z"
    }
  ]
}
```

#### ERROR (0x03) — Server → Client

```json
{
  "ref_message_id": 12345,
  "error_code": 1,
  "description": "Node bunker-42 already registered"
}
```

`error_code` is the numeric code from the table above. `description` is human-readable and may vary between implementations.

#### QUERY_NODE (0x04) — Client → Server

```json
{
  "node_id": "bunker-42"
}
```

#### LIST_NODES (0x05) — Client → Server

Payload length = 0. No JSON body.

#### HEARTBEAT (0x06) — Client → Server

Payload length = 0. No JSON body. Server does NOT respond. Absence of heartbeat within `EOP_HEARTBEAT_INTERVAL × 2` marks the node as OFFLINE.

### Versioning Policy

| Change Type | Classification | Required Action |
|-------------|---------------|-----------------|
| Add new message_type | **Non-breaking** | ADR-003 addendum |
| Add optional field to existing payload | **Non-breaking** | ADR-003 addendum |
| Add required field to existing payload | **Breaking** | Increment `protocol_version` + new ADR |
| Remove or rename existing field | **Breaking** | Increment `protocol_version` + new ADR |
| Change field data type | **Breaking** | Increment `protocol_version` + new ADR |
| Change envelope structure | **Breaking** | Increment `protocol_version` + new ADR |
| Change framing mechanism | **Breaking** | Increment `protocol_version` + new ADR |
| Change error_code semantics | **Breaking** | Increment `protocol_version` + new ADR |

Receivers MUST ignore unknown fields in JSON payloads (forward compatibility). This allows optional fields to be added without breaking existing implementations.

## Alternatives Considered

| Alternative | Reason Rejected |
|-------------|----------------|
| Delimiter-based framing (`\n`) | Fails when payload contains the delimiter (v0.2 graphs). Escaping adds fragility. Stream desynchronization on any escaping bug is irrecoverable. |
| Protobuf encoding | Contradicts ADR-001 spirit. Adds shared `.proto` dependency. Not human-debuggable. |
| Custom binary encoding | Interoperability risk across C++/C/Go implementations. Endianness, padding, alignment bugs are likely. JSON eliminates this class of errors. |
| No `message_id` field | Impossible to correlate responses in concurrent operations. Impossible to deduplicate in v1.0 edge ingestor. |

## Consequences

**Positive:**
- The protocol is fully documented. Any engineer can implement a compatible client from this ADR
- Length-prefix framing is deterministic and supports any payload content including binary (future-proof)
- JSON encoding enables debugging with standard tools from day one
- The versioning policy provides a clear contract for protocol evolution

**Negative:**
- JSON is verbose compared to binary. A 100K-node graph as JSON is ~50 MB vs ~20 MB binary. Acceptable for v0.2 given network is local
- Two `recv()` calls per message (header + payload) vs one scan for delimiter. Negligible overhead
- `message_id` must be tracked per-connection (monotonically increasing counter). Minor bookkeeping

**Constraints:**
- All implementations MUST handle partial `recv()` returns for both the frame length prefix and the frame body
- All implementations MUST use network byte order (big-endian) for multi-byte header fields via `htonl()`/`ntohl()`
- JSON parsing library is a required dependency in all languages (cJSON for C, nlohmann/json for C++, encoding/json for Go)
