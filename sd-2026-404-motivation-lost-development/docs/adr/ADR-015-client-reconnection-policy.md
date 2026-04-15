# ADR-015: Client Reconnection Policy — Explicit API, Caller-Owned Retry

| Field | Value |
|-------|-------|
| **Date** | 11-04-2026 |
| **Status** | Accepted |
| **Lab** | v0.1 |
| **Deciders** | Squad 404-motivation-lost |

## Context

US-109 requires that after a pod crash and automatic Kubernetes restart, a client can reconnect and re-register without manual operator intervention (AC5, AC6). This makes reconnection a first-class concern of the client library.

The library already provides `eop_connect()` for initial connection and `eop_send_command()` for request-response exchanges. When the server pod crashes, the client-side TCP socket becomes dead: the next `eop_send_command()` call fails with `EOP_ERR_DISCONNECTED`. The question is: who drives recovery from that point?

Two forces are in tension:

1. **Simplicity for the caller**: ideally the library hides the reconnection detail — the caller retries the operation and it just works.
2. **Transparency and control**: reconnection has observable side effects (latency spike, session state reset, mandatory re-registration). Hiding it inside `eop_send_command()` means the caller cannot distinguish a transient retry from a successful first attempt, cannot apply backoff, and cannot know that the server lost all node state.

The EOP protocol is stateful: after a pod restart the server's `NodeRegistry` is empty. A client that was registered before the crash is no longer registered. Any transparent reconnect that does not also re-register would silently put the client in a broken state where subsequent `QUERY_NODE` or `HEARTBEAT` commands reference a node the server does not know about.

## Decision

The client library exposes reconnection as an **explicit, synchronous API call** (`eop_reconnect()`). The caller is fully responsible for:

1. Detecting the disconnection (`eop_last_error()` returns `EOP_ERR_DISCONNECTED`).
2. Calling `eop_reconnect()` to re-establish the TCP connection.
3. Re-sending `EOP_REGISTER` to restore server-side node state before resuming normal operations.

`eop_reconnect()` reuses the same `eop_client_t*` handle (host and port are stored internally) and performs a non-blocking `connect()` guarded by `poll()` with a configurable timeout. It does not automatically re-register the node — that is left to the caller.

The library does **not** attempt automatic reconnection inside `eop_send_command()` or any other function.

## Alternatives Considered

| Alternative | Reason Rejected |
|-------------|----------------|
| **Transparent auto-reconnect inside `eop_send_command()`** | The protocol is stateful: reconnecting without re-registering leaves the server unaware of the node. A transparent reconnect would silently succeed at the TCP level but subsequent commands would fail with `ERR_NOT_FOUND`. Hiding reconnection also prevents the caller from applying backoff or limiting retry attempts. |
| **Transparent auto-reconnect + automatic re-registration** | Requires the library to store the original `REGISTER` payload indefinitely. Introduces ambiguity: if a command fails and the library re-registers and retries internally, did the server receive the original command once or twice? The EOP protocol has no idempotency guarantee on `REGISTER` (duplicate returns `ERR_DUPLICATE`). |
| **No reconnect support in the library** | Forces every caller to open a new `eop_connect()` handle and manage the lifecycle manually, losing the stored host/port and requiring handle replacement. More disruptive to caller code than an explicit `eop_reconnect()`. |

## Consequences

**Positive:**
- The library is predictable: `eop_send_command()` never silently retries or reconnects. A `NULL` return always means the caller must take explicit action.
- Reconnection policy (retry interval, exponential backoff, max attempts) is fully owned by the application layer, not hardcoded in the library.
- The two-step recovery sequence (reconnect → re-register) is visible in application code, making post-mortem analysis straightforward.
- No risk of silent duplicate registration: the caller decides when and whether to re-register.

**Negative:**
- Every caller that wants resilience must implement a reconnect-and-reregister loop. There is no "just works" fallback.
- If a caller forgets to re-register after reconnecting, subsequent commands fail with `ERR_NOT_FOUND` — a subtle bug with no library-level guard.

**Constraints Introduced:**
- Any application embedding the client library that requires pod-restart resilience MUST implement the following pattern after any `EOP_ERR_DISCONNECTED` error:
  ```c
  eop_reconnect(client, timeout_ms);           // re-establish TCP
  eop_send_command(client, EOP_REGISTER, ...); // restore server state
  ```
- Integration tests that validate reconnection behavior MUST exercise `eop_reconnect()` from the C client library. A test that opens a raw socket (Python or shell) or calls only `eop_connect()` does not validate AC6 or AC7.
- Future extensions to `eop_reconnect()` (e.g., built-in retry with backoff) MUST remain opt-in via parameters to preserve backward compatibility with callers that implement their own retry logic.

## Supplementary Notes

### Recommended retry parameters

`eop_reconnect()` returns immediately on failure; the caller owns the retry loop. The following parameters are recommended (and used by `eop_reconnect_probe`):

| Parameter | Recommended value | Rationale |
|-----------|------------------|-----------|
| Initial delay | 500 ms | Avoids hammering the server during `initialDelaySeconds` |
| Backoff multiplier | 2× | Standard exponential backoff |
| Maximum delay | 5 s | Caps worst-case wait per attempt |
| Jitter | ±10 % of delay | Prevents thundering-herd when multiple clients reconnect simultaneously |
| Max attempts | 10 | ~30 s total; covers K8s `initialDelaySeconds` (2 s) + `failureThreshold × periodSeconds` (3 × 10 s = 30 s) |

Callers that implement their own retry loop SHOULD align the total retry window with the Kubernetes liveness probe configuration (`initialDelaySeconds + failureThreshold × periodSeconds`) so that reconnect attempts do not exhaust before the pod is marked Ready.

### `next_msg_id` behavior after reconnect

`eop_reconnect()` does **not** reset the handle's `next_msg_id` counter. Message IDs continue incrementing from where they left off before the disconnection. This is intentional: the EOP protocol (ADR-003) does not require message ID monotonicity or continuity across sessions. Callers and tests MUST NOT assume that the first message sent after `eop_reconnect()` carries ID 0.

### Thread safety of `eop_reconnect()`

`eop_reconnect()` acquires the handle's internal mutex for the entire duration of the reconnect operation. Because `eop_send_command()` also holds that mutex while blocked on I/O, calling `eop_reconnect()` from a second thread while another thread is blocked inside `eop_send_command()` on the same handle will deadlock.

**Rule**: `eop_reconnect()` MUST NOT be called concurrently with `eop_send_command()` on the same handle. The expected usage pattern is:

1. A single owner thread detects `EOP_ERR_DISCONNECTED` from `eop_send_command()`.
2. That same thread calls `eop_reconnect()` (no other thread is using the handle at this point).
3. After `eop_reconnect()` returns `EOP_OK`, normal concurrent usage may resume.

This constraint is documented in the `@warning` on `eop_reconnect()` in `eop_client.h`.
