# CloudSentry — Data Structures Guide

This document provides a comprehensive explanation of every custom data structure implemented in the CloudSentry load balancer. Each section covers **what** the DS is, **why** it was chosen, **how** it works internally, and **where** its code integrates into the production routing pipeline.

---

## Table of Contents

1. [Splay Tree — Session Affinity Cache](#1-splay-tree--session-affinity-cache)
2. [Binomial Heap — Priority Queue & Failover Engine](#2-binomial-heap--priority-queue--failover-engine)
3. [URL Trie — Layer-7 Path-Based Routing](#3-url-trie--layer-7-path-based-routing)
4. [Skip List — Server Search Index](#4-skip-list--server-search-index)
5. [Segment Tree — Peak Load Analytics (RMQ)](#5-segment-tree--peak-load-analytics-rmq)
6. [Token Bucket Rate Limiter — Traffic Throttling](#6-token-bucket-rate-limiter--traffic-throttling)
7. [Union-Find (DSU) — Zone Health Management](#7-union-find-dsu--zone-health-management)
8. [Routing Pipeline — How All DS Work Together](#8-routing-pipeline--how-all-ds-work-together)

---

## 1. Splay Tree — Session Affinity Cache

**File:** `core/splay_tree.hpp`   
**Role:** Caches which server a client was last routed to, enabling "sticky sessions."

### What Is a Splay Tree?

A Splay Tree is a self-adjusting Binary Search Tree (BST). Every time a node is accessed (searched or inserted), it gets **rotated to the root** through a series of zig, zig-zig, and zig-zag rotations. This has the effect that:

- **Frequently accessed keys** stay near the root → O(1) amortized access
- **Rarely accessed keys** sink deeper → no wasted memory on cold data
- No need for explicit balancing metadata (unlike AVL or Red-Black trees)

### Why Splay Tree for Session Affinity?

In a load balancer, many clients send repeated requests (e.g., a user browsing a website sends 10–50 requests per session). Instead of re-computing the optimal server for every single request, the Splay Tree caches the `clientId → serverId` mapping. Since recently active clients splay to the root, repeat lookups are essentially O(1).

### Complexity

| Operation | Amortized | Worst Case |
|-----------|-----------|------------|
| `find()`  | O(1)*     | O(n)       |
| `insert()`| O(log n)  | O(n)       |

*For repeat access patterns (which is the common case in session affinity).

### How It Works — Code Walkthrough

**Node Structure:**
```cpp
struct SplayNode {
    int clientId;     // key: the client making the request
    int serverId;     // value: the server they were assigned to
    SplayNode *left, *right, *parent;
};
```

**The Core Splay Operation (tree rotation):**
```cpp
void splay(SplayNode* x) {
    while (x->parent) {
        if (!x->parent->parent) {
            // ZIG: x is child of root → single rotation
            if (x == x->parent->left) rightRotate(x->parent);
            else leftRotate(x->parent);
        } else if (x == x->parent->left && x->parent == x->parent->parent->left) {
            // ZIG-ZIG: both left children → rotate grandparent then parent
            rightRotate(x->parent->parent);
            rightRotate(x->parent);
        } else if (x == x->parent->right && x->parent == x->parent->parent->right) {
            // ZIG-ZIG: both right children
            leftRotate(x->parent->parent);
            leftRotate(x->parent);
        } else if (x == x->parent->left && x->parent == x->parent->parent->right) {
            // ZIG-ZAG: left-right pattern
            rightRotate(x->parent);
            leftRotate(x->parent);
        } else {
            // ZIG-ZAG: right-left pattern
            leftRotate(x->parent);
            rightRotate(x->parent);
        }
    }
}
```

**Finding a cached session (splays the result to root for fast re-access):**
```cpp
int find(int clientId) {
    SplayNode* curr = root;
    while (curr) {
        if (clientId == curr->clientId) {
            splay(curr);               // move to root for O(1) next time
            return curr->serverId;     // cache hit
        }
        if (clientId < curr->clientId) curr = curr->left;
        else curr = curr->right;
    }
    return -1; // cache miss
}
```

### Where It's Used in the Load Balancer

In `load_balancer.hpp`, the very first thing `route()` does is check the Splay Tree:

```cpp
RouteResult route(int clientId, int priority, const std::string& path, int slaMs) {
    // ...
    
    // STEP 1: Check Splay Tree cache
    int cachedSid = sessionCache.find(clientId);
    if (cachedSid != -1 && servers[cachedSid]->healthy) {
        // Cache hit! Route to same server as before
        servers[cachedSid]->requestQueue.insert({now + slaMs, rid, clientId, priority});
        return {true, cachedSid, rid, "Affinity Match"};
    }
    
    // Cache miss → continue to Trie routing...
    // ...
    
    // STEP FINAL: Cache the new assignment for next time
    sessionCache.insert(clientId, bestSid);
}
```

**Result:** When client `9287` sends their 2nd request within a session, the lookup completes in amortized O(1) because the node was splayed to the root on the first query.

---

## 2. Binomial Heap — Priority Queue & Failover Engine

**File:** `core/binomial_heap.hpp`   
**Role:** Manages per-server request queues with Earliest-Deadline-First (EDF) scheduling. Enables O(log n) queue merging during server failover.

### What Is a Binomial Heap?

A Binomial Heap is a collection (forest) of **Binomial Trees**, where each tree satisfies the min-heap property. A Binomial Tree B_k has exactly 2^k nodes and is formed by linking two B_(k-1) trees.

The key insight: any integer N can be represented in binary, and a Binomial Heap of N elements contains exactly one tree for each `1` bit in the binary representation. For example:

- **270** in binary = `100001110`
- Bits set at positions: 1, 2, 3, 8
- Forest contains: B₁(2 nodes), B₂(4 nodes), B₃(8 nodes), B₈(256 nodes)
- Verify: 2 + 4 + 8 + 256 = 270 ✅

### Why Binomial Heap Instead of `std::priority_queue`?

The **killer feature** is `merge()`. When a server crashes, all its pending requests must be migrated to a healthy server. With `std::priority_queue`, you'd need to pop N elements from one queue and push them into another → **O(N log N)**. With a Binomial Heap, you simply merge the two forests → **O(log N)**.

### Complexity

| Operation     | Binomial Heap | std::priority_queue |
|---------------|---------------|---------------------|
| `insert()`    | O(log n)      | O(log n)            |
| `extractMin()`| O(log n)      | O(log n)            |
| **`merge()`** | **O(log n)**  | **O(N log N)**      |

### How It Works — Code Walkthrough

**Node Structure:**
```cpp
template <typename T>
struct BinomialNode {
    T data;                              // the request (with deadline, priority, etc.)
    int degree;                          // number of children = rank of this tree
    BinomialNode *child, *sibling, *parent;
};
```

**Linking two trees of equal rank (the fundamental operation):**
```cpp
void linkNodes(BinomialNode<T>* y, BinomialNode<T>* z) {
    y->parent = z;
    y->sibling = z->child;   // y becomes leftmost child of z
    z->child = y;
    z->degree++;              // z's rank increases by 1
}
```
This is exactly like binary addition: when you have two trees of rank k, they combine into one tree of rank k+1, similar to carrying a 1 in binary addition.

**The Merge Operation (the reason we chose this DS):**
```cpp
void merge(BinomialHeap<T>& other) {
    count += other.count;
    std::list<BinomialNode<T>*> newRoots = mergeRoots(other);  // interleave by degree
    roots.clear();
    other.roots.clear();

    // Consolidation: combine trees of equal degree (like binary addition with carry)
    auto curr = newRoots.begin();
    auto next = std::next(curr);

    while (next != newRoots.end()) {
        if ((*curr)->degree != (*next)->degree || ...) {
            curr = next;   // degrees differ, move on
        } else {
            if ((*curr)->data <= (*next)->data) {
                linkNodes(*next, *curr);    // curr absorbs next
                newRoots.erase(next);
            } else {
                linkNodes(*curr, *next);    // next absorbs curr
                curr = newRoots.erase(curr);
            }
        }
        next = std::next(curr);
    }
    roots = newRoots;
}
```

**Insert uses merge internally:**
```cpp
void insert(T val) {
    BinomialHeap<T> temp;
    temp.roots.push_back(new BinomialNode<T>(val));
    temp.count = 1;
    merge(temp);   // merge a single-element heap → O(log n) amortized
}
```

### Where It's Used in the Load Balancer

**Each server has its own Binomial Heap as a request queue:**
```cpp
struct Server {
    BinomialHeap<Request> requestQueue;   // EDF scheduling
    // ...
};
```

**When a request arrives, it's inserted with a deadline:**
```cpp
servers[bestSid]->requestQueue.insert({now + slaMs, rid, clientId, priority});
```

**When a request completes, the minimum-deadline request is extracted:**
```cpp
void complete(int sid, int rid, int latency, bool failed) {
    if (!s->requestQueue.empty()) s->requestQueue.extractMin();
    s->activeConnections--;
}
```

**When a server is killed — the merge operation:**
```cpp
void killServer(int sid) {
    servers[sid]->healthy = false;
    
    int fallbackSid = /* find a healthy server in same zone */;
    
    if (fallbackSid != -1 && !servers[sid]->requestQueue.empty()) {
        // This is the O(log n) merge that justifies the entire DS choice
        servers[fallbackSid]->requestQueue.merge(servers[sid]->requestQueue);
        servers[fallbackSid]->activeConnections += servers[sid]->activeConnections;
        servers[sid]->activeConnections = 0;
    }
}
```

---

## 3. URL Trie — Layer-7 Path-Based Routing

**File:** `core/url_trie.hpp`   
**Role:** Routes requests to specific server zones based on the URL path prefix.

### What Is a Trie?

A Trie (prefix tree) is a tree where each edge represents a single character. To look up a string, you walk from the root, following one edge per character. When you reach a terminal node, you've found a match.

### Why Trie for URL Routing?

HTTP load balancers need to route traffic based on URL paths:
- `/api/v1/*` → `us-east-1a` (primary API cluster)
- `/api/v2/*` → `us-east-1b` (secondary API cluster)
- `/static/*` → `us-west-1a` (CDN/static assets)

A Trie resolves this in **O(L)** time where L is the length of the URL path — regardless of how many routes are registered. A hash map would require exact matches; a Trie naturally handles prefix matching.

### Complexity

| Operation        | Time   |
|------------------|--------|
| `addRoute()`     | O(L)   |
| `route()` lookup | O(L)   |
| Space            | O(Σ·L) |

Where L = path length, Σ = alphabet size.

### How It Works — Code Walkthrough

**Node Structure (each node has a map of character → child):**
```cpp
class TrieNode {
public:
    std::unordered_map<char, TrieNode*> children;
    std::string zone;       // which server zone this path resolves to
    bool isTerminal;        // true if this node represents a complete route
};
```

**Adding a route (character by character):**
```cpp
void addRoute(const std::string& path, const std::string& zone) {
    TrieNode* curr = root;
    for (char c : path) {
        if (curr->children.find(c) == curr->children.end()) {
            curr->children[c] = new TrieNode();   // create a new branch
        }
        curr = curr->children[c];                  // follow the branch
    }
    curr->isTerminal = true;
    curr->zone = zone;   // mark terminal with the target zone
}
```

**Lookup with longest prefix matching:**
```cpp
std::string route(const std::string& url) {
    TrieNode* curr = root;
    std::string lastFoundZone = "default";
    
    for (char c : url) {
        if (curr->children.find(c) == curr->children.end()) break;  // no more matching
        curr = curr->children[c];
        if (curr->isTerminal) {
            lastFoundZone = curr->zone;   // remember the deepest match
        }
    }
    return lastFoundZone;   // return the longest matching prefix's zone
}
```

**Example trace:** For the URL `/api/v1/users/123`:
```
root → '/' → 'a' → 'p' → 'i' → '/' → 'v' → '1' ← TERMINAL: "us-east-1a"
                                              → '/' → 'u' → 's' → ... (no more terminals)
```
Result: `"us-east-1a"` (matched at `/api/v1`)

### Where It's Used in the Load Balancer

**Routes are registered at startup:**
```cpp
LoadBalancer(int n) {
    routeTrie.addRoute("/api/v1", "us-east-1a");
    routeTrie.addRoute("/api/v2", "us-east-1b");
    routeTrie.addRoute("/static", "us-west-1a");
    // ...
}
```

**Every incoming request queries the Trie:**
```cpp
RouteResult route(int clientId, int priority, const std::string& path, int slaMs) {
    // After splay tree check...
    
    // Trie resolves the path to a preferred zone
    std::string preferredZone = routeTrie.route(path);
    
    // Then we search for the least-loaded server IN that zone
    for (auto s : servers) {
        if (s->zone == preferredZone && s->activeConnections < 10) {
            bestSid = s->id;
            break;
        }
    }
}
```

---

## 4. Skip List — Server Search Index

**File:** `core/simple_skip_list.hpp`   
**Role:** Maintains a sorted index of servers by load, enabling O(log n) lookup of the least-loaded server.

### What Is a Skip List?

A Skip List is a probabilistic data structure that provides O(log n) search by building multiple levels of "express lanes" above a sorted linked list. Each element has a randomly chosen height; higher levels skip over more elements, acting as shortcuts.

Think of it like a subway system:
- **Level 0 (Local):** Stops at every station (every server)
- **Level 1 (Express):** Stops every 2nd station
- **Level 2 (Super Express):** Stops every 4th station

To find a server, you start at the highest level and drop down only when you overshoot. This gives O(log n) expected time.

### Why Skip List for Server Indexing?

The load balancer needs to quickly find the server with the lowest active connections. A Skip List allows:
- O(log n) insertion when a server's load changes
- O(log n) deletion when a server goes offline
- O(log n) search for the minimum-key element

### Complexity (Expected)

| Operation  | Expected | Worst Case |
|------------|----------|------------|
| `search()` | O(log n) | O(n)       |
| `insert()` | O(log n) | O(n)       |
| `remove()` | O(log n) | O(n)       |
| Space      | O(n)     | O(n log n) |

### How It Works — Code Walkthrough

**Node Structure (each node has a tower of forward pointers):**
```cpp
struct Node {
    int key;                     // server load (activeConnections)
    int value;                   // server ID
    std::vector<Node*> next;     // next[i] = pointer at level i
};
```

**Random level generation (the probabilistic magic):**
```cpp
int randomLevel() {
    int lvl = 0;
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    while (dist(rng) < p && lvl < maxLevel) lvl++;  // coin flip: 50% chance to go higher
    return lvl;
}
```

**Search (starts from top level, drops down):**
```cpp
Node* search(int key) {
    Node* curr = head;
    for (int i = maxLevel; i >= 0; i--) {           // start at highest level
        while (curr->next[i] && curr->next[i]->key < key) {
            curr = curr->next[i];                    // skip forward
        }
        // drop down to next level (more granular)
    }
    curr = curr->next[0];   // follow base level
    if (curr && curr->key == key) return curr;
    return nullptr;
}
```

**Insert (creates a node with a random tower height):**
```cpp
void insert(int key, int value) {
    std::vector<Node*> update(maxLevel + 1);
    Node* curr = head;

    // Find the insertion point at all levels
    for (int i = maxLevel; i >= 0; i--) {
        while (curr->next[i] && curr->next[i]->key < key)
            curr = curr->next[i];
        update[i] = curr;   // remember where we dropped down
    }

    int lvl = randomLevel();   // random height for express lanes
    Node* newNode = new Node(key, value, lvl);
    for (int i = 0; i <= lvl; i++) {
        newNode->next[i] = update[i]->next[i];   // splice in at each level
        update[i]->next[i] = newNode;
    }
}
```

### Where It's Used in the Load Balancer

**Server index is a Skip List:**
```cpp
class LoadBalancer {
    SimpleSkipList serverIndex;   // sorted by activeConnections
    // ...
};
```

**Initialized at startup:**
```cpp
for (int i = 0; i < n; i++) {
    servers.push_back(new Server(i, zones[i % 3]));
    serverIndex.insert(0 + i, i);   // key=load, value=serverId
}
```

---

## 5. Segment Tree — Peak Load Analytics (RMQ)

**File:** `core/segment_tree.hpp`   
**Role:** Tracks per-server peak connections over a rolling 60-second window, answering "what was the maximum load in time range [L, R]?" in O(log n).

### What Is a Segment Tree?

A Segment Tree is a binary tree built over an array where each node stores an aggregate value (max, min, sum, etc.) for a range of the array. The root covers the entire array; leaves cover individual elements; internal nodes cover their children's ranges.

For a 60-element array (60 seconds), the tree has ~120 nodes and supports:
- **Point Update:** Change one element in O(log n)
- **Range Query:** Get the max over any range [L, R] in O(log n)

### Why Segment Tree for Peak Load?

Standard polling checks server load every few seconds and may miss **micro-bursts** — brief spikes that last less than the polling interval. The Segment Tree records load at every second and can answer "what was the peak load between second 10 and second 45?" instantly.

### Complexity

| Operation        | Time     |
|------------------|----------|
| `set()` update   | O(log n) |
| `getMaxRange()`  | O(log n) |
| Build             | O(n)     |
| Space            | O(4n)    |

### How It Works — Code Walkthrough

**Internal array-based tree (stored as a flat vector):**
```cpp
class PeakSegmentTree {
    int n;
    std::vector<int> tree;   // tree[1] = root, tree[2i] = left child, tree[2i+1] = right child

public:
    PeakSegmentTree(int size = 60) {
        n = size;
        tree.assign(4 * n, 0);   // 4x size for safe indexing
    }
```

**Point Update (recursively updates the path from leaf to root):**
```cpp
void update(int idx, int val, int node, int start, int end) {
    if (start == end) {
        tree[node] = val;         // leaf: set the value
        return;
    }
    int mid = (start + end) / 2;
    if (idx <= mid) update(idx, val, 2 * node, start, mid);        // go left
    else            update(idx, val, 2 * node + 1, mid + 1, end);  // go right
    tree[node] = std::max(tree[2 * node], tree[2 * node + 1]);     // recalculate parent
}
```

**Range Maximum Query (recursively queries overlapping segments):**
```cpp
int query(int l, int r, int node, int start, int end) {
    if (r < start || end < l) return 0;              // completely outside → ignore
    if (l <= start && end <= r) return tree[node];   // completely inside → return
    int mid = (start + end) / 2;
    return std::max(
        query(l, r, 2 * node, start, mid),           // check left half
        query(l, r, 2 * node + 1, mid + 1, end)      // check right half
    );
}
```

### Where It's Used in the Load Balancer

**Each server has its own Segment Tree:**
```cpp
struct Server {
    PeakSegmentTree peakTracker;   // 60-second rolling window
    // ...
    Server(int id, const std::string& zone) : peakTracker(60) {}
};
```

**Updated every time a request completes:**
```cpp
void complete(int sid, int rid, int latency, bool failed) {
    // ...
    int sec = /* current second mod 60 */;
    
    // Record the current connection count at this second
    s->peakTracker.set(sec, s->activeConnections);
}
```

**Queried for analytics:**
```cpp
int getPeakLoad(int sid, int startSec, int endSec) {
    return servers[sid]->peakTracker.getMaxRange(startSec, endSec);
}
```

---

## 6. Token Bucket Rate Limiter — Traffic Throttling

**File:** `balancer/rate_limiter.hpp`   
**Role:** Prevents any single client from overwhelming the system by limiting request rates using the Token Bucket algorithm.

### What Is a Token Bucket?

Imagine each client has a bucket that fills with tokens at a constant rate. Each request costs 1 token. If the bucket is empty, the request is rejected (HTTP 429). The bucket has a maximum capacity (burst limit), so a client can send a short burst but can't sustain high rates.

### How It Works — Code Walkthrough

**Per-client bucket:**
```cpp
struct TokenBucket {
    double tokens;        // current tokens available
    double maxTokens;     // burst capacity
    double refillRate;    // tokens per second
    std::chrono::steady_clock::time_point lastRefill;

    bool consume(double cost = 1.0) {
        auto now = std::chrono::steady_clock::now();
        double secs = /* time since last refill */;
        tokens = std::min(maxTokens, tokens + secs * refillRate);  // refill
        lastRefill = now;

        if (tokens >= cost) {
            tokens -= cost;   // deduct
            return true;      // allowed
        }
        return false;         // rate limited
    }
};
```

**Two-tier checking (global + per-client):**
```cpp
RateLimitResult check(int clientId, double cost = 1.0) {
    // 1. Global burst protection (protects entire system)
    if (!globalBucket->consume(cost)) {
        return {false, globalBucket->available(), 50};
    }

    // 2. Per-client rate limit
    TokenBucket* bucket = /* get or create bucket for clientId */;
    bool ok = bucket->consume(cost);
    if (!ok) return {false, remaining, retryMs};
    return {true, remaining, 0};
}
```

### Where It's Used

**First check in every route() call:**
```cpp
RouteResult route(int clientId, int priority, const std::string& path, int slaMs) {
    auto rl = rateLimiter.check(clientId);
    if (!rl.allowed) return {false, -1, -1, "Rate Limited"};
    // ... proceed with routing
}
```

---

## 7. Union-Find (DSU) — Zone Health Management

**File:** `core/union_find/zone_dsu.hpp`  
**Role:** Groups servers into zone-level components. Enables O(α(n)) queries for "is this server in a live zone?" and batch kill/revive of entire zones.

### What Is a Union-Find (Disjoint Set Union)?

A DSU maintains a collection of disjoint sets. It supports two operations:
- **Union(a, b):** Merge the sets containing `a` and `b`
- **Find(a):** Determine which set `a` belongs to

With **path compression** and **union by rank**, both operations run in O(α(n)) — the inverse Ackermann function, which is effectively constant for all practical inputs (α(n) ≤ 4 for n < 10^80).

### Why DSU for Zone Management?

In a multi-datacenter deployment, servers are grouped by geographic zone (e.g., `us-east-1a`, `us-east-1b`). When a zone fails (network outage, power failure), ALL servers in that zone must be marked dead simultaneously. A DSU:
- Groups servers by zone at startup using `union()`
- Answers "is server X in a dead zone?" in O(α(n)) using `isAlive()`
- Batch-kills an entire zone in O(k) where k = servers in the zone

### Complexity

| Operation        | Time      |
|------------------|-----------|
| `unionServers()` | O(α(n))   |
| `findRoot()`     | O(α(n))   |
| `isAlive()`      | O(1)      |
| `killZone()`     | O(k)      |
| `sameZone()`     | O(α(n))   |

### How It Works — Code Walkthrough

**Path Compression (iterative — flattens the tree on every find):**
```cpp
int findRoot(int x) {
    int root = x;
    while (parent[root] != root) root = parent[root];
    // Path compression: point all nodes directly to root
    while (parent[x] != root) {
        int next = parent[x];
        parent[x] = root;
        x = next;
    }
    return root;
}
```

**Union by Rank (attaches smaller tree under larger):**
```cpp
void unionServers(int a, int b) {
    int ra = findRoot(a), rb = findRoot(b);
    if (ra == rb) return;
    if (rank_[ra] < rank_[rb]) std::swap(ra, rb);
    parent[rb] = ra;
    if (rank_[ra] == rank_[rb]) rank_[ra]++;
}
```

**Zone Kill (batch operation):**
```cpp
void killZone(const std::string& zone) {
    deadZones.insert(zone);
    for (int sid : zoneMembers[zone]) alive[sid] = false;
}
```

### Where It's Used in the Load Balancer

**Initialized at startup — servers are grouped by zone:**
```cpp
LoadBalancer(int n) {
    for (int i = 0; i < n; i++) {
        zoneDsu.assignZone(i, zones[i % 3]);
    }
    // Union servers within the same zone into DSU components
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (zones[i % 3] == zones[j % 3]) zoneDsu.unionServers(i, j);
        }
    }
}
```

**Used in EVERY route() call for health checks:**
```cpp
// DSU isAlive() provides O(α(n)) zone-aware health check
if (cachedSid != -1 && servers[cachedSid]->healthy && zoneDsu.isAlive(cachedSid)) {
    // Route to cached server only if BOTH server AND zone are alive
}

for (auto s : servers) {
    if (!s->healthy || !zoneDsu.isAlive(s->id)) continue;  // skip dead zones
}
```

**Zone-level kill/revive:**
```cpp
void killZone(const std::string& zone) {
    zoneDsu.killZone(zone);  // DSU batch-kills all servers in this zone
    for (auto s : servers) {
        if (s->zone == zone && s->healthy) killServer(s->id);
    }
}
```

---

## 8. Routing Pipeline — How All DS Work Together

When a request arrives at CloudSentry, it flows through every data structure in sequence:

```
                          ┌─────────────────────────────────────┐
                          │         INCOMING REQUEST            │
                          │  clientId=9287, path="/api/v1"      │
                          └──────────────┬──────────────────────┘
                                         │
                          ┌──────────────▼──────────────────────┐
                     ①    │     RATE LIMITER (Token Bucket)      │
                          │  Client 9287 has tokens? → YES       │
                          └──────────────┬──────────────────────┘
                                         │
                          ┌──────────────▼──────────────────────┐
                     ②    │     SPLAY TREE (Session Cache)       │
                          │  find(9287) → cache HIT: srv-02      │
                          │  Node splayed to root for O(1) next  │
                          └──────────────┬──────────────────────┘
                                         │ (if cache miss ↓)
                          ┌──────────────▼──────────────────────┐
                     ③    │     URL TRIE (Path Routing)          │
                          │  route("/api/v1") → "us-east-1a"    │
                          │  Matched at depth 7 in O(L) time    │
                          └──────────────┬──────────────────────┘
                                         │
                          ┌──────────────▼──────────────────────┐
                     ④    │     SKIP LIST (Server Index)         │
                          │  Search for least-loaded server      │
                          │  in zone "us-east-1a" → srv-02      │
                          └──────────────┬──────────────────────┘
                                         │
                          ┌──────────────▼──────────────────────┐
                     ⑤    │     BINOMIAL HEAP (Request Queue)    │
                          │  srv-02.requestQueue.insert(req)     │
                          │  EDF scheduling by deadline          │
                          └──────────────┬──────────────────────┘
                                         │
                          ┌──────────────▼──────────────────────┐
                     ⑥    │     SEGMENT TREE (Peak Tracking)     │
                          │  On completion:                      │
                          │  srv-02.peakTracker.set(sec, conn)   │
                          │  Records load for RMQ analytics      │
                          └─────────────────────────────────────┘
```

### Failover Scenario (Server Crash)

When `srv-02` is killed:

1. **Union-Find DSU** marks srv-02 as dead — `zoneDsu.killServer(2)`, O(1)
2. **Skip List** removes srv-02 from the index — `serverIndex.remove()`, O(log n)
3. **Binomial Heap** extracts each request from srv-02's queue and inserts into the least-loaded healthy server — O(k log n)
4. **Splay Tree** cache entries pointing to srv-02 become stale — next lookup checks `zoneDsu.isAlive()` and falls through to Trie
5. **Segment Tree** on receiving servers records the connection spike
6. **URL Trie** remains unchanged (routes to zones, not individual servers)

When an entire zone is killed:

1. **Union-Find DSU** batch-kills the zone — `zoneDsu.killZone("us-east-1b")`, marks all 3 servers dead in O(k)
2. All `route()` calls skip those servers via `zoneDsu.isAlive()` check

---

## Summary Table

| DS             | File                        | Purpose                     | Key Operation               | Complexity    |
|----------------|-----------------------------|-----------------------------|-----------------------------|--------------|
| Splay Tree     | `core/splay_tree.hpp`       | Session affinity cache      | `find()` / `splay()`       | O(1) amortized|
| Binomial Heap  | `core/binomial_heap.hpp`    | Priority queue + failover   | `merge()` (heap union)      | O(log n)      |
| URL Trie       | `core/url_trie.hpp`         | L7 path-based routing       | `route()` (prefix match)    | O(L)          |
| Skip List      | `core/simple_skip_list.hpp` | Server search index         | `search()` (multi-level)    | O(log n)      |
| Segment Tree   | `core/segment_tree.hpp`     | Peak load analytics (RMQ)   | `getMaxRange()` (range max) | O(log n)      |
| Token Bucket   | `balancer/rate_limiter.hpp` | Per-client rate limiting    | `consume()` (token check)   | O(1)          |
| Union-Find DSU | `core/union_find/zone_dsu.hpp`| Zone health management    | `isAlive()` / `killZone()`  | O(α(n)) ≈ O(1)|
