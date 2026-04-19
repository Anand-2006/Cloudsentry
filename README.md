# CloudSentry — Dynamic Load Balancer

A production-grade dynamic load balancer built from scratch in C++, with 6 custom data structures,
a FastAPI bridge, and a React dashboard. Built as a college DS course project.

---

## Project Structure

```
CloudSentry/
│
├── core/                          # Pure DS implementations
│   ├── skip_list/
│   │   ├── coarse_lock.hpp        # Global mutex skip list (benchmark baseline)
│   │   ├── fine_grained.hpp       # Per-node lock skip list (optimistic locking)
│   │   └── lock_free.hpp          # CAS atomic skip list (production router)
│   ├── binomial_heap/
│   │   └── binomial_heap.hpp      # O(log n) merge heap — failover queue merger
│   ├── rb_tree/
│   │   └── request_rbtree.hpp     # EDF request queue — cancel/reprioritize support
│   ├── segment_tree/
│   │   └── metrics_segtree.hpp    # Lazy propagation — sliding window metrics
│   └── union_find/
│       └── zone_dsu.hpp           # Path compression DSU — availability zone clusters
│
├── balancer/
│   ├── load_balancer.hpp          # Main router — all DS wired together
│   └── rate_limiter.hpp           # Token bucket — per-client rate limiting
│
├── benchmarks/
│   └── skip_list_bench.cpp        # 3 workloads × 4 thread counts + Binomial Heap merge bench
│
├── api/
│   └── main.py                    # FastAPI bridge — exposes C++ sim state as REST
│
├── frontend/
│   └── index.html                 # Standalone React dashboard — no npm needed
│
├── main.cpp                       # 7-phase simulation driver
├── CMakeLists.txt                 # CMake build (optional)
└── README.md
```

---

## Data Structures

| DS | File | Role | Complexity |
|---|---|---|---|
| Lock-Free Skip List | `core/skip_list/lock_free.hpp` | Server routing index | O(log n) |
| Fine-Grained Skip List | `core/skip_list/fine_grained.hpp` | Benchmark comparison | O(log n) |
| Coarse Lock Skip List | `core/skip_list/coarse_lock.hpp` | Benchmark baseline | O(log n) |
| Red-Black Tree | `core/rb_tree/request_rbtree.hpp` | Per-server EDF request queue | O(log n) |
| Binomial Heap | `core/binomial_heap/binomial_heap.hpp` | O(log n) failover queue merge | O(log n) |
| Segment Tree + Lazy Prop | `core/segment_tree/metrics_segtree.hpp` | Sliding window metrics | O(log n) |
| Union-Find (DSU) | `core/union_find/zone_dsu.hpp` | Zone cluster management | O(α(n)) |
| Token Bucket | `balancer/rate_limiter.hpp` | Per-client rate limiting | O(1) |

---

## Build & Run

### Prerequisites
```bash
# Linux — install g++ and pthreads
sudo apt install g++ build-essential

# Python (for API only)
pip install fastapi uvicorn --break-system-packages
```

### C++ Simulation
```bash
# build main simulation
g++ -std=c++17 -O2 -pthread -Wall -I. main.cpp -o cloudsentry

# run 7-phase simulation
./cloudsentry
```

### Skip List Benchmark
```bash
# build benchmark
g++ -std=c++17 -O2 -pthread -I. benchmarks/skip_list_bench.cpp -o bench_skiplist

# run — outputs throughput table + Binomial Heap merge vs naive comparison
./bench_skiplist
```

### FastAPI Bridge
```bash
cd api
uvicorn main:app --reload --port 8000
# API available at http://localhost:8000
# Docs at http://localhost:8000/docs
```

### Frontend Dashboard
```bash
# no npm, no install — just open directly
xdg-open frontend/index.html    # Linux
open frontend/index.html         # macOS
# or just drag into any browser
```

The dashboard tries `http://localhost:8000` for live data.
If the API isn't running it falls back to a live mock simulation automatically.

---

## Simulation Phases

The `main.cpp` simulation runs 7 phases to demonstrate every DS:

| Phase | What happens | DS demonstrated |
|---|---|---|
| 1 | Normal traffic — 8 clients × 60 requests | Lock-Free Skip List routing, RB-Tree enqueue |
| 2 | Abusive clients hammering 200 requests | Token Bucket rate limiting |
| 3 | Priority routing + mid-flight cancellations | RB-Tree removeById, updateDeadline |
| 4 | Zone `us-east-1a` killed (3 servers down) | DSU zone kill, Binomial Heap O(log n) merge |
| 4b | Traffic under zone failure | Skip List reroutes to surviving 6 servers |
| 5 | Zone revived | DSU re-enables component |
| 6 | Single server kill + circuit breaker | Circuit breaker OPEN → HALF_OPEN → CLOSED |
| 7 | Reprioritize a pending request | RB-Tree updateDeadline |

---

## API Endpoints

| Method | Endpoint | Description |
|---|---|---|
| GET | `/status` | All server states, throughput, events |
| POST | `/zone/kill/{zone}` | Kill entire availability zone |
| POST | `/zone/revive/{zone}` | Revive availability zone |
| POST | `/server/kill/{id}` | Kill individual server |
| POST | `/server/revive/{id}` | Revive individual server |
| POST | `/spike` | Trigger manual traffic spike |
| GET | `/bench` | Run skip list benchmark (async) |
| GET | `/zones` | Zone list and colors |

---

## Benchmark Results (actual run)

### Skip List — Throughput (ops/sec)

**Write Heavy (60% insert / 30% remove / 10% query)**

| Threads | CoarseLock | FineGrained | LockFree |
|---|---|---|---|
| 1 | 483,092 | 1,443,001 | 1,594,896 |
| 2 | 1,752,848 | 1,343,183 | 2,034,588 |
| 4 | 1,343,183 | 1,685,630 | 2,152,853 |
| 8 | 627,353 | 896,258 | 973,710 |

**Read Heavy (10% insert / 5% remove / 85% query)**

| Threads | CoarseLock | FineGrained | LockFree |
|---|---|---|---|
| 8 | **147,365** | 2,080,624 | **4,006,009** |

CoarseLock collapses at 8 threads read-heavy (54ms vs LockFree's 2ms).

### Binomial Heap — O(log n) Merge vs O(n log n) Naive

| Queue Size | Merge ops/s | Naive ops/s | Speedup |
|---|---|---|---|
| n=100 | 200,000,000 | 25,000,000 | 8× |
| n=1000 | 2,000,000,000 | 38,461,538 | 52× |
| n=5000 | 2,000,000,000 | 24,390,244 | ~82× |
