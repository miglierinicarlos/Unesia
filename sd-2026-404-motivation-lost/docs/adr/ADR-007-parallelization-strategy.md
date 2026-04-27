# ADR-007: Parallelization Strategy — OpenMP

| Field | Value |
|-------|-------|
| **Date** | Week 5 / 2026-03-30 |
| **Status** | Accepted |
| **Lab** | v0.2 |
| **Deciders** | Eng-A, Eng-B, Eng-C |

## Context

The HPC engine introduced in v0.2 must execute three graph algorithms — Dijkstra (shortest path), connected components, and degree centrality — over graphs of up to 100K nodes and edges. The NFRs mandate measurable speedup over serial execution: ≥ 2× at 4 threads on 50K-node graphs and ≥ 3× at 8 threads on 100K-node graphs (NFR-1).

Multiple parallelization mechanisms are available in a C++17 POSIX environment: OpenMP pragmas, POSIX pthreads, C++11 `std::thread` with manual work distribution, Intel TBB, and GPU offload via CUDA or OpenCL.

The HPC engine is a standalone process that already communicates with the v0.1 server via the EOP socket protocol (ADR-008). The parallelization mechanism is internal to the engine and invisible to the server and clients; it affects only the engine's compute loop.

The thread count must be configurable at runtime — different K8s pod sizes expose different CPU limits — without recompilation.

## Decision

We will use **OpenMP** for parallelization. The HPC engine links against the system OpenMP runtime via `find_package(OpenMP REQUIRED)` in CMake and `target_link_libraries(...OpenMP::OpenMP_CXX)`. Thread count is controlled exclusively via the `OMP_NUM_THREADS` environment variable, injected through the K8s ConfigMap (per ADR-006). No thread count is hardcoded.

Parallelism is applied at the algorithm level:

- **Dijkstra**: parallel relaxation loop using thread-local min-heaps and a reduction phase over atomic distance array (`std::atomic<uint32_t>`).
- **Connected components**: parallel BFS frontier expansion with `schedule(guided)` to handle irregular frontier sizes; atomic visited flags (`std::atomic<uint8_t>`) prevent races on component assignment.
- **Degree centrality**: embarrassingly parallel — each node's degree is computed independently with `schedule(static)`.

All input graph data is read-only after CSR construction. No mutable state is shared inside parallel regions, which is the primary guarantee for ThreadSanitizer compliance.

## Alternatives Considered

| Alternative | Reason Rejected |
|-------------|----------------|
| POSIX pthreads | Requires manual thread lifecycle, work queues, and barrier synchronization for every algorithm. High implementation cost with no correctness advantage over OpenMP for data-parallel loops. |
| `std::thread` + manual work distribution | Same objection as pthreads. Standard library threads do not provide loop-level work distribution; the team would implement a thread pool on top, which is already present in the v0.1 server for a different purpose. |
| Intel TBB | Provides higher-level parallel constructs (`tbb::parallel_for`, `tbb::parallel_reduce`) but adds an external dependency not present in the lab environment. OpenMP is available on all target systems without additional installation. |
| CUDA / GPU offload | Requires NVIDIA hardware and CUDA toolchain in the CI environment and in the K8s cluster. Not a constraint the platform can guarantee. Adds significant build complexity. |
| C++17 Parallel STL (`std::execution::par`) | Implementation quality varies by platform. On Linux with libstdc++, requires Intel TBB as a backend, reintroducing that dependency. |

## Consequences

**Positive:**
- OpenMP is supported by GCC, Clang, and MSVC with a single `#pragma omp` directive. No change to the build system beyond `find_package(OpenMP REQUIRED)`.
- Thread count is a runtime parameter (`OMP_NUM_THREADS`). The same binary runs correctly at 1, 4, 8, or `nproc` threads without recompilation, which is required for K8s resource scaling.
- Measured speedups (benchmark report, 2026-04-15): pipeline speedup of **2.95× at 4 threads on 50K nodes** and **3.51× at 8 threads on 100K nodes**, satisfying NFR-1.
- ThreadSanitizer reports zero data races at all thread configurations from 1 to `nproc`, verified in CI.

**Negative:**
- Dijkstra's algorithm has a high serial fraction due to the priority queue coordination between threads. Speedup at 4 threads on 50K nodes is 0.745× for Dijkstra in isolation; the composite pipeline speedup target is met only because connected components parallelizes well (6.80× at 8 threads on 50K nodes).
- Degree centrality shows overhead-dominated behavior on graphs below 50K nodes — parallelization produces negative returns at small sizes. The benchmark report documents the minimum graph size at which parallelization is beneficial per algorithm.
- OpenMP's `default(none)` clause is mandatory on all parallel regions to prevent accidental shared-state captures. This adds verbosity to pragma lines.

**Constraints Introduced:**
- All `#pragma omp parallel` regions must specify `default(none)` and explicitly list shared and private variables. Omitting this clause is a Clang-Tidy violation and blocks merge.
- The input graph (CSR representation) must be fully constructed and immutable before any parallel region is entered. Mutating the graph inside a parallel region is prohibited.
- `OMP_NUM_THREADS` must never be hardcoded in source or Dockerfiles. It is always injected via the K8s ConfigMap (ADR-006). Setting it directly in code is prohibited.
- The CI pipeline runs ThreadSanitizer on all thread configurations from 1 to `$(nproc)`. A TSan finding at any thread count blocks merge (NFR-3).
- The benchmark target (`make benchmark`) must remain reproducible: fixed random seed, fixed graph topology, five runs per configuration. The Benchmark Report (`docs/benchmark/benchmark_report.md`) is a committed artifact updated on every HPC engine change.
