# HPC Engine Benchmark Report

## Benchmark Environment

### Hardware Specifications

- **CPU:** 12th Gen Intel(R) Core(TM) i5-12450H
- **Physical cores:** 12
- **Max OpenMP threads:** 12
- **Memory:** 16093044 kB
- **Kernel:** 6.8.0-106-generic
- **CPU governor:** powersave

### Compiler & Build

- **Compiler:** clang++ 18.1.3
- **Flags:** `-O3 -march=native`
- **OpenMP:** 202011

### Measurement Details

- **Runs per configuration:** 5
- **Graph topology:** Power-Law (seed=42, hub\_fraction=0.05, hub\_degree=4096)

---

### Configuration Details
- **Seed:** 42
- **Runs per config:** 5
- **Hub fraction:** 0.05
- **Hub degree:** 4096

## 1. Per-Algorithm Breakdown

### Graph Size: 10000 nodes

| Nodes | Threads | Algorithm | Avg (ms) | Speedup |
|-------|---------|-----------|----------|---------|
| 10000 | 1 | `dijkstra` | 23.645 | **1.000x** |
| 10000 | 1 | `connected_components` | 3.700 | **1.000x** |
| 10000 | 1 | `degree_centrality` | 0.016 | **1.000x** |
| 10000 | 2 | `dijkstra` | 30.071 | **0.786x** |
| 10000 | 2 | `connected_components` | 2.100 | **1.762x** |
| 10000 | 2 | `degree_centrality` | 0.026 | **0.639x** |
| 10000 | 4 | `dijkstra` | 29.806 | **0.793x** |
| 10000 | 4 | `connected_components` | 1.948 | **1.899x** |
| 10000 | 4 | `degree_centrality` | 0.142 | **0.115x** |
| 10000 | 8 | `dijkstra` | 38.584 | **0.613x** |
| 10000 | 8 | `connected_components` | 1.575 | **2.349x** |
| 10000 | 8 | `degree_centrality` | 0.103 | **0.158x** |

> **Amdahl Analysis @ 10000 nodes, 8 threads:**
>
> * **`dijkstra`**: speedup: 0.50x | serial fraction: _N/A (overhead-dominated)_
> * **`connected_components`**: speedup: 2.41x | serial fraction: **0.3316**
> * **`degree_centrality`**: speedup: 0.15x | serial fraction: _N/A (overhead-dominated)_

<br>

### Graph Size: 50000 nodes

| Nodes | Threads | Algorithm | Avg (ms) | Speedup |
|-------|---------|-----------|----------|---------|
| 50000 | 1 | `dijkstra` | 130.589 | **1.000x** |
| 50000 | 1 | `connected_components` | 25.235 | **1.000x** |
| 50000 | 1 | `degree_centrality` | 0.069 | **1.000x** |
| 50000 | 2 | `dijkstra` | 147.218 | **0.887x** |
| 50000 | 2 | `connected_components` | 12.703 | **1.987x** |
| 50000 | 2 | `degree_centrality` | 0.177 | **0.392x** |
| 50000 | 4 | `dijkstra` | 175.389 | **0.745x** |
| 50000 | 4 | `connected_components` | 7.604 | **3.319x** |
| 50000 | 4 | `degree_centrality` | 0.148 | **0.469x** |
| 50000 | 8 | `dijkstra` | 170.873 | **0.764x** |
| 50000 | 8 | `connected_components` | 3.856 | **6.544x** |
| 50000 | 8 | `degree_centrality` | 0.262 | **0.265x** |

> **Amdahl Analysis @ 50000 nodes, 8 threads:**
>
> * **`dijkstra`**: speedup: 1.48x | serial fraction: **0.6319**
> * **`connected_components`**: speedup: 6.80x | serial fraction: **0.0251**
> * **`degree_centrality`**: speedup: 0.38x | serial fraction: _N/A (overhead-dominated)_

<br>

### Graph Size: 100000 nodes

| Nodes | Threads | Algorithm | Avg (ms) | Speedup |
|-------|---------|-----------|----------|---------|
| 100000 | 1 | `dijkstra` | 140.437 | **1.000x** |
| 100000 | 1 | `connected_components` | 31.450 | **1.000x** |
| 100000 | 1 | `degree_centrality` | 0.075 | **1.000x** |
| 100000 | 2 | `dijkstra` | 149.136 | **0.942x** |
| 100000 | 2 | `connected_components` | 17.702 | **1.777x** |
| 100000 | 2 | `degree_centrality` | 0.114 | **0.663x** |
| 100000 | 4 | `dijkstra` | 154.517 | **0.909x** |
| 100000 | 4 | `connected_components` | 10.903 | **2.885x** |
| 100000 | 4 | `degree_centrality` | 0.087 | **0.864x** |
| 100000 | 8 | `dijkstra` | 177.839 | **0.790x** |
| 100000 | 8 | `connected_components` | 8.032 | **3.916x** |
| 100000 | 8 | `degree_centrality` | 0.256 | **0.295x** |

> **Amdahl Analysis @ 100000 nodes, 8 threads:**
>
> * **`dijkstra`**: speedup: 0.79x | serial fraction: _N/A (overhead-dominated)_
> * **`connected_components`**: speedup: 3.88x | serial fraction: **0.1515**
> * **`degree_centrality`**: speedup: 0.26x | serial fraction: _N/A (overhead-dominated)_

<br>

## 2. Pipeline Speedup
*Parallel workload executing `connectedComponents` + `degreeCentrality`.*

| Nodes | Threads | Pipeline (ms) | Speedup |
|-------|---------|---------------|---------|
| 10000 | 1 | 1.925 | **1.000x** |
| 10000 | 2 | 1.089 | **1.767x** |
| 10000 | 4 | 0.772 | **2.493x** |
| 10000 | 8 | 0.584 | **3.297x** |
| 50000 | 1 | 13.856 | **1.000x** |
| 50000 | 2 | 8.056 | **1.720x** |
| 50000 | 4 | 4.697 | **2.950x** |
| 50000 | 8 | 4.001 | **3.463x** |
| 100000 | 1 | 30.466 | **1.000x** |
| 100000 | 2 | 19.583 | **1.556x** |
| 100000 | 4 | 11.313 | **2.693x** |
| 100000 | 8 | 8.684 | **3.508x** |

## 3. AC Validation

- [x] **AC2** (>=2.0x at 50K with 4 threads): 2.950x
- [x] **AC3** (>=3.0x at 100K with 8 threads): 3.508x
