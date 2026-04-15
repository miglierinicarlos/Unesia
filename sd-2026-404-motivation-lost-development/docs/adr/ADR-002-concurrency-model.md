# ADR-002: Server Concurrency Model

| Field | Value |
|-------|-------|
| **Date** | 21-03-2026 |
| **Status** | Accepted |
| **Lab** | v0.1 |
| **Deciders** | Squad 404-motivation-lost |

## Context

The EOP C++ server must support a minimum of 10 concurrent clients without degradation (NFR-1) and pass a load test with 1000 clients sending 10,000 requests each (US-102 AC8). When a client sends a message, the server must process it without blocking other connected clients.

Three primary concurrency models are available in the POSIX environment. This ADR analyzes all three with quantitative trade-offs specific to EOP's requirements.

## Options Analysis

### Option A — Thread-per-Connection

One OS thread is created per accepted client connection. The thread handles all communication for that client and exits when the connection closes.

```
accept() → pthread_create() → thread handles client → thread exits on disconnect
```

**Memory Model:**
Each thread consumes a stack (default: 8 MB on Linux, configurable via `pthread_attr_setstacksize`). With a reduced stack of 2 MB:
- 100 clients = 200 MB of committed stack
- 1000 clients = 2 GB of committed stack
- 10,000 clients = 20 GB — physically unviable on most machines

**Synchronization:**
Per-client state is thread-local (no synchronization needed). Shared state (NodeRegistry) requires a mutex or reader-writer lock. Contention point is limited to registry operations (REGISTER, QUERY_NODE, LIST_NODES).

**Quantitative Assessment:**

| Metric | Value |
|--------|-------|
| Memory per client | 2–8 MB (stack) |
| Max practical clients | ~500 (limited by stack memory) |
| Thread creation overhead | ~50 µs per `pthread_create` |
| Implementation complexity | Low |
| Synchronization surface | Small (only shared registry) |
| US-102 AC8 (1000 clients) | **RISK: marginal on 2 MB stacks, fails on 8 MB** |
| US-102 AC4 (fd leak check) | Easy to verify — 1 thread per fd |

**Verdict:** Simple to implement. Marginal for the 1000-client load test unless stacks are aggressively reduced. Thread creation/destruction overhead becomes noticeable under churn.

---

### Option B — Thread Pool

A fixed pool of N worker threads processes client requests. Accepted connections are placed in a thread-safe work queue. Workers dequeue and handle one request at a time.

```
accept() → enqueue(fd) → worker dequeues → handles request → returns to pool
```

**Memory Model:**
Thread count is fixed at startup (configurable via `EOP_THREAD_POOL_SIZE`). Memory footprint is bounded:
- Pool of 16 threads × 2 MB stack = 32 MB total (constant regardless of client count)
- 1000 clients share 16 threads. Each client's state is on the heap (~1 KB per connection context)
- Total for 1000 clients: 32 MB (threads) + 1 MB (contexts) = ~33 MB

**Synchronization:**
The work queue is the critical synchronization point. Requires:
- `std::mutex` + `std::condition_variable` for producer-consumer pattern
- The accept loop is the producer; worker threads are consumers
- NodeRegistry still needs its own reader-writer lock for shared state

**Quantitative Assessment:**

| Metric | Value |
|--------|-------|
| Memory per client | ~1 KB (heap-allocated context) |
| Memory total (fixed) | 32 MB for 16 threads |
| Max practical clients | ~10,000 (limited by fd count, not threads) |
| Request routing overhead | ~1 µs (mutex lock + condition signal) |
| Implementation complexity | Medium |
| Synchronization surface | Medium (work queue + registry) |
| US-102 AC8 (1000 clients) | **PASS: 33 MB total, well within limits** |
| US-102 AC4 (fd leak check) | Requires careful fd ownership tracking between queue and workers |

**Pool sizing considerations:**
- CPU-bound work (v0.2 HPC): pool size = number of CPU cores
- I/O-bound work (v0.1 socket handling): pool size = 2× to 4× CPU cores
- Recommended starting point: `EOP_THREAD_POOL_SIZE = 16` (adjustable via ConfigMap)

**Verdict:** Predictable memory footprint. Handles 1000 clients comfortably. The work queue adds complexity but is a well-understood pattern. Pool size is a tuning parameter that can be adjusted without code changes.

---

### Option C — I/O Multiplexing (epoll)

A single thread (or small number of threads) monitors multiple file descriptors simultaneously using `epoll`. Processing is event-driven: the thread is notified when an fd is ready to read or write.

```
epoll_create() → epoll_ctl(ADD, fd) → epoll_wait() → process ready fds → loop
```

**Memory Model:**
Near-zero per-connection overhead. Each connection is a file descriptor plus application-level state (~1 KB).
- 1000 clients: ~1 MB total
- 10,000 clients: ~10 MB total
- 100,000 clients: ~100 MB total

**Synchronization:**
In a single-threaded epoll loop, there is NO shared state between connections — all processing is sequential within the event loop. This eliminates data races entirely. However:
- Long-running operations (v0.2 graph analysis) block the entire event loop
- Multi-threaded epoll (thread pool of epoll workers) reintroduces synchronization complexity

**Quantitative Assessment:**

| Metric | Value |
|--------|-------|
| Memory per client | ~1 KB (fd + state struct) |
| Max practical clients | 100,000+ (kernel limited) |
| Event dispatch overhead | ~0.1 µs per ready fd |
| Implementation complexity | **High** |
| Synchronization surface | None (single-thread) / High (multi-thread) |
| US-102 AC8 (1000 clients) | **PASS: trivially** |
| US-102 AC4 (fd leak check) | Centralized fd management simplifies tracking |

**Critical complexity factor:**
Each connection requires an explicit state machine:

```c
enum ConnState { READING_HEADER, READING_PAYLOAD, PROCESSING, WRITING_RESPONSE };
struct Connection {
    int fd;
    ConnState state;
    uint8_t headerBuf[10];
    size_t headerBytesRead;
    std::vector<uint8_t> payloadBuf;
    size_t payloadBytesRead;
    // ... response buffers, write position, etc.
};
```

Every `epoll_wait` return requires checking which state each fd is in and advancing it. Partial reads/writes must be tracked explicitly per connection. This is what nginx, Redis, and Node.js do — but they are mature codebases with years of refinement.

**Verdict:** Superior scalability. Near-zero memory overhead. But the implementation complexity is 3–4× higher than thread pool. The state machine per connection is the primary risk: bugs in state transitions cause silent data corruption. For a 3-week sprint, this is the highest-risk option.

---

## Comparative Summary

| Criterion | Thread-per-conn | Thread Pool | epoll |
|-----------|:-:|:-:|:-:|
| Memory (1000 clients) | ~2 GB | ~33 MB | ~1 MB |
| Max clients (practical) | ~500 | ~10,000 | 100,000+ |
| Implementation time (est.) | 2–3 days | 4–5 days | 8–10 days |
| Data race risk | Low | Medium | None (single) / High (multi) |
| US-102 AC8 (1000 × 10K req) | ⚠️ Marginal | ✅ Pass | ✅ Pass |
| US-102 AC2 (latency 2× check) | ✅ Good | ✅ Good | ✅ Best |
| US-102 AC6 (TSan zero races) | ✅ Easy | ⚠️ Queue needs care | ✅ Trivial (single-thread) |
| v0.2 compatibility (HPC dispatch) | ✅ Simple | ✅ Simple | ⚠️ Blocking dispatch stalls loop |
| Sprint risk (3 weeks total) | ✅ Low | ✅ Moderate | ❌ High |

## Decision

### Thread Pool
The server uses a fixed-size thread pool with a thread-safe work queue. Pool size is configurable via EOP_THREAD_POOL_SIZE environment variable (default: 16). The accept loop enqueues connections; worker threads dequeue and process. This model balances implementation complexity with scalability, passing the 1000-client load test comfortably within ~33 MB total memory.

## Consequences

*To be filled after the decision is made:*

- Synchronization model: The work queue uses std::mutex + std::condition_variable in a producer-consumer pattern. The lock is held only during enqueue/dequeue, never during message processing. The NodeRegistry uses std::shared_mutex (reader-writer lock): reads (QUERY_NODE, LIST_NODES) take a shared lock and execute concurrently; writes (REGISTER, OFFLINE transitions) take an exclusive lock. Simple counters (m_activeConnections, m_nextSessionId) use std::atomic.
- Shared state invariants: After an ACK is sent in response to a REGISTER, the node is guaranteed visible to all threads. The work queue guarantees every enqueued fd is processed by exactly one worker — no duplication, no loss. Fd ownership transfers from the accept loop to the worker at dequeue time; RAII ensures cleanup on any exit path.
- Concurrent load testing: TSan is executed in CI against the full test suite including 100 concurrent clients with mixed operations (US-102 AC6). The 1000-client load test (US-102 AC8) verifies zero server errors and zero client timeouts. Fd leak detection compares /proc/PID/fd counts before and after 1M connect/disconnect cycles (US-102 AC4).
- Practical limits: With a pool of 16 threads, the server supports ~10,000 simultaneous connections (limited by fd count, not by the pool). Latency increases proportionally to the client/worker ratio under saturation, but no client receives an error unless EOP_MAX_CLIENTS is exceeded. Pool size is tunable via EOP_THREAD_POOL_SIZE without recompilation.
- v0.2 compatibility: The EngineDispatcher integrates as an additional handler alongside the existing ones. A worker receiving ANALYZE_GRAPH forwards it to the HPC engine and waits for the result synchronously. Since graph analysis requests are infrequent relative to registry operations, pool saturation is not a concern. The pool size does not need to increase for v0.2.
