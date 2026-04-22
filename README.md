# CloudSentry: High-Performance Load Balancing & Persistence Engine

CloudSentry is a production-inspired, distributed load balancer designed to solve the critical challenges of modern cloud infrastructure: **Traffic Steering**, **Sub-millisecond Session Persistence**, and **Instant Failover Resilience**. 

By orchestrating a hybrid architecture of five advanced data structures, CloudSentry ensures that high-volume traffic is distributed with mathematical precision while maintaining 100% availability during hardware failure.

---

## 🌪️ The Problem
In high-scale environments, standard load balancers often struggle with:
1. **Slow Re-routing**: Moving thousands of pending requests one by one when a server dies ($O(N)$ overhead).
2. **Cache Locality**: Repeating expensive lookups for the same client multiple times.
3. **Analytics Latency**: Calculating traffic peaks in real-time requires expensive database range queries.

## 🛡️ The CloudSentry Solution
CloudSentry addresses these by implementing a **5-Layer Core Engine**, where every operation—from routing to failover—is optimized through specialized data structures.

---

## 🧬 Deep Dive: The 5-Layer Engine

### 1. Persistence Layer | Splay Tree (Unit 1)
*   **Role**: Sub-millisecond Session Affinity.
*   **Logic**: Instead of a standard hash map, CloudSentry uses a **Splay Tree** to store `clientId -> serverId` mappings.
*   **Optimization**: Most recently active clients are "Splayed" to the root. If a client sends a burst of requests, subsequent lookups occur in **$O(1)$ amortized time**, drastically reducing routing overhead for loyal users.

### 2. Distributed Queue | Binomial Heap (Unit 2)
*   **Role**: High-Availability Request Management.
*   **Logic**: Every server's internal queue is managed by a custom **Binomial Heap** implementing Earliest Deadline First (EDF) scheduling.
*   **Innovation**: The **Union Operation**. In a standard heap, merging two queues takes $O(N)$ time. CloudSentry’s Binomial Heap merges entire request volumes in **$O(\log N)$** time, enabling near-instantaneous failover migration.

### 3. L7 Traffic Router | URL Trie (Unit 3)
*   **Role**: Prefix-Based Path Steering.
*   **Logic**: Uses a custom **Trie (Prefix Tree)** to match incoming URL paths (e.g., `/api/v1/user`) to specific backend server groups.
*   **Efficiency**: Matches are determined in **$O(L)$ time** (where $L$ is path length), ensuring that routing speed never degrades, regardless of how many routes are added to the system.

### 4. Global Load Indexer | Simple Skip List (Unit 4)
*   **Role**: Real-Time Server Discovery.
*   **Logic**: Maintains a probabilistic, multi-level **Skip List** of all active servers, sorted by their current connection load.
*   **Optimization**: Using randomization and leveled pointers, the orchestrator can find and select the least-loaded server in **$O(\log N)$** time without the heavy re-balancing costs of Red-Black trees.

### 5. Forecasting Engine | Segment Tree (Unit 5)
*   **Role**: Aggregated Range Analytics.
*   **Logic**: Maintains performance metrics in a **Segment Tree** structure.
*   **Analytics**: Supports **Range Maximum Queries (RMQ)**. It can report the peak load experienced by any server in any specific time window (e.g., "Max load between 10s and 45s of the current minute") in **$O(\log N)$**.

---

## 🚀 Getting Started

### 1. Prerequisites
- **C++17 Compiler** (GCC/Clang)
- **CMake** (3.10+)
- **Python 3.10+**
- **pip** (FastAPI, Uvicorn)

### 2. Build the Core Engine
```bash
mkdir build && cd build
cmake ..
make
```

### 3. Launch the Management Console
CloudSentry features a React-powered real-time dashboard.
```bash
cd api
pip install fastapi uvicorn
uvicorn main:app --reload --port 8000
```
Then, visit: **`http://localhost:8000`**

---

## 📈 Innovation Highlights
*   **Logarithmic Scaling**: Every core operation—including failover—scales at $O(\log n)$ or better, making the system viable for thousands of servers.
*   **Zero-Copy Failover**: The Binomial Heap's unique merging property allows the system to migrate thousands of "stuck" requests during a server crash without individual re-processing.
*   **Predictive Diagnostics**: The Segment Tree-based analytics allow the system to detect "micro-bursts" that standard 5-second polling intervals would miss.

---
**Developed by CloudSentry Engineering Core**
