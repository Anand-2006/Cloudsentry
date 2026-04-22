# CloudSentry (Simplified)

A dynamic load balancer implementation focused on core **Data Structures**. This project is designed as a 2nd-year undergraduate DS project.

## Project Goal
To demonstrate how classical data structures (Skip Lists, Priority Queues, Sliding Windows) can be combined to solve a real-world problem: balancing network traffic across multiple servers.

## Key Data Structures Used

| DS | Implementation | Role |
|---|---|---|
| **Skip List** | `core/simple_skip_list.hpp` | Efficiently searching and managing server load. |
| **Priority Queue** | `std::priority_queue` (STL) | **Earliest Deadline First (EDF)** request scheduling. |
| **Sliding Window** | `core/simple_metrics.hpp` | Tracking error rates and latency over time. |
| **Queue Merge** | `std::priority_queue` | Merging pending requests during server failover. |

## Features
- **Dynamic Routing**: Automatically picks the server with the least connections.
- **Failover Support**: If a server or zone fails, pending requests are automatically moved to a healthy server.
- **Rate Limiting**: Prevents any single client from overwhelming the system.
- **Circuit Breaker**: Temporarily stops sending traffic to servers that are returning too many errors.

## Project Structure
```
CloudSentry/
├── core/
│   ├── simple_skip_list.hpp  # Custom Skip List
│   └── simple_metrics.hpp    # Sliding window performance tracking
├── balancer/
│   ├── load_balancer.hpp     # Main Logic (wiring everything together)
│   └── rate_limiter.hpp      # Token Bucket rate limiter
└── main.cpp                  # Simulation driver
```

## How to Run
```bash
# Compile
g++ -std=c++17 main.cpp -o cloudsentry

# Run
./cloudsentry
```

## Why this is a 2nd Year project
1. It implements a **Skip List** from scratch (a common advanced DS topic).
2. It uses **STL containers** correctly (`priority_queue`, `vector`, `unordered_map`).
3. It uses a **single-mutex** approach for thread safety, which is appropriate for this level.
4. It focuses on **clean code and logic** rather than production-grade concurrency.
