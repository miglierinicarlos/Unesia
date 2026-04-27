# ADR-008: HPC Engine Integration Pattern

| Field | Value                   |
|-------|-------------------------|
| **Date** | Week 6                  |
| **Status** | Accepted                |
| **Lab** | v0.2                    |
| **Deciders** | All squads + Instructor |

## Context
The HPC engine introduced in v0.2 could be designed as a standalone process with its own client interface, or as an integrated component of the
existing EOP communication layer. This decision determines whether v0.2 builds on v0.1 or replaces it. 

## Decision 
The HPC engine registers itself as an EOP node with the v0.1 server and receives work via the existing socket protocol. 

The integration pattern is:
1. The HPC engine starts and sends a REGISTER message to the v0.1 server (same as any EOP client).
2. When a C client sends an ANALYZE_GRAPH message to the v0.1 server, the server dispatches the request to the registered HPC engine
node.
3. The engine processes the graph and sends the result back to the server.
4. The server forwards the result to the originating client. 

The ANALYZE_GRAPH message type is added to the protocol as a non-breaking extension (per ADR-003’s versioning policy). Its definition is an
addendum to ADR-003. 

## Alternatives Considered

| Alternative                   | Reason Rejected                                                                                                                                                                     |
|-------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Standalone HPC process with its own socket server | Breaks the continuity of the product. The client would need to know about a separate endpoint. The v0.1 server would not be involved in the flow, severing the thread of continuity |
| HPC engine as a library linked into the v0.1 server | Creates a monolith. The engine cannot be scaled independently. Eliminates the microservice pattern.                                                                                 |
| gRPC for engine communication | Inconsistent with ADR-001. Introduces a different communication mechanism for a subset of services without a strong justification.                                                  |

## Consequences
- The thread of continuity is enforced architecturally: the HPC engine cannot be demonstrated without the v0.1 server running. End-to-end
integration tests must traverse the full path.
- OTel spans must cover the full dispatch chain: client request → server dispatch → engine processing → result delivery. All spans must share
the same trace_id.
- If the HPC engine is offline, the v0.1 server must return ERROR_SERVICE_UNAVAILABLE to the client. The server must not crash or hang
waiting for an unavailable engine.
- The engine can run as a K8s Job (one-shot batch processing) or as a K8s Deployment (persistent availability). Both modes are supported
and documented.
