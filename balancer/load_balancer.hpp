#pragma once
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <chrono>
#include <iostream>
#include <unordered_map>
#include <queue>
#include <algorithm>
#include "../core/simple_skip_list.hpp"
#include "../core/simple_metrics.hpp"
#include "rate_limiter.hpp"

// Simple request structure
struct Request {
    long long deadlineMs;
    int requestId;
    int clientId;
    int priority;
    
    // For priority queue (Earliest Deadline First)
    bool operator>(const Request& other) const {
        return deadlineMs > other.deadlineMs;
    }
    // For Binomial Heap ordering
    bool operator<(const Request& other) const {
        return deadlineMs < other.deadlineMs;
    }
    bool operator<=(const Request& other) const {
        return deadlineMs <= other.deadlineMs;
    }
};
#include "../core/binomial_heap.hpp"
#include "../core/url_trie.hpp"
#include "../core/splay_tree.hpp"
#include "../core/segment_tree.hpp"
#include "../core/union_find/zone_dsu.hpp"

enum class CircuitState { CLOSED, OPEN, HALF_OPEN };

struct Server {
    int id;
    std::string zone;
    int activeConnections = 0;
    int totalRequests = 0;
    bool healthy = true;
    CircuitState circuitState = CircuitState::CLOSED;
    long long lastOpenTime = 0; 
    
    // Custom Binomial Heap (Unit 2) instead of STL priority_queue
    BinomialHeap<Request> requestQueue;
    ServerMetrics* metrics;
    PeakSegmentTree peakTracker; // Unit 5: Range Max Queries

    Server(int id, const std::string& zone) : id(id), zone(zone), peakTracker(60) {
        metrics = new ServerMetrics(id);
    }
    ~Server() { delete metrics; }
};

class LoadBalancer {
    std::vector<Server*> servers;
    SimpleSkipList serverIndex; 
    URLRouter routeTrie;
    SessionCache sessionCache;
    ZoneDSU zoneDsu;          // Union-Find for zone health management
    RateLimiter rateLimiter;
    std::mutex lbMutex;
    
    int reqIdCounter = 0;

    long long getNowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void updateIndex(Server* s) {
        serverIndex.insert(s->activeConnections * 1000 + s->id, s->id);
    }

public:
    LoadBalancer(int n, double rlMax = 50.0, double rlRate = 15.0) 
        : zoneDsu(n), rateLimiter(rlMax, rlRate) {
        
        routeTrie.addRoute("/api/v1", "us-east-1a");
        routeTrie.addRoute("/api/v2", "us-east-1b");
        routeTrie.addRoute("/static", "us-west-1a");

        std::vector<std::string> zones = {"us-east-1a", "us-east-1b", "us-west-1a"};
        for (int i = 0; i < n; i++) {
            servers.push_back(new Server(i, zones[i % 3]));
            serverIndex.insert(0 + i, i);
            // Register server in DSU for zone awareness
            zoneDsu.assignZone(i, zones[i % 3]);
        }
        // Union servers within the same zone into DSU components
        for (int i = 0; i < n; i++) {
            for (int j = i + 1; j < n; j++) {
                if (zones[i % 3] == zones[j % 3]) zoneDsu.unionServers(i, j);
            }
        }
    }

    ~LoadBalancer() {
        for (auto s : servers) delete s;
    }

    struct RouteResult {
        bool accepted;
        int serverId;
        int requestId;
        std::string reason;
    };

    // PRODUCTION CORE: Advanced Traffic Steering & Persistence
    RouteResult route(int clientId, int priority, const std::string& path = "/default", int slaMs = 2000) {
        std::lock_guard<std::mutex> lock(lbMutex);

        auto rl = rateLimiter.check(clientId);
        if (!rl.allowed) return {false, -1, -1, "Rate Limited"};

        long long now = getNowMs();

        // SESSION PERSISTENCE ENGINE (Client Caching)
        // Sub-millisecond lookup for repeat clients using amortized optimization.
        int cachedSid = sessionCache.find(clientId);
        // DSU isAlive() provides O(α(n)) zone-aware health check
        if (cachedSid != -1 && cachedSid < (int)servers.size() && servers[cachedSid]->healthy && zoneDsu.isAlive(cachedSid) && servers[cachedSid]->circuitState != CircuitState::OPEN) {
            int rid = reqIdCounter++;
            servers[cachedSid]->requestQueue.insert({now + slaMs, rid, clientId, priority});
            servers[cachedSid]->activeConnections++;
            updateIndex(servers[cachedSid]);
            return {true, cachedSid, rid, "Affinity Match"};
        }

        // LAYER-7 ROUTING (Path-Based Steering)
        std::string preferredZone = routeTrie.route(path);

        int bestSid = -1;
        int bestZoneSid = -1;
        for (auto s : servers) {
            if (!s->healthy || !zoneDsu.isAlive(s->id)) continue;
            if (s->circuitState == CircuitState::OPEN) {
                if (now - s->lastOpenTime > 5000) s->circuitState = CircuitState::HALF_OPEN;
                else continue;
            }
            // Track the least-loaded server in the preferred zone
            if (s->zone == preferredZone) {
                if (bestZoneSid == -1 || s->activeConnections < servers[bestZoneSid]->activeConnections)
                    bestZoneSid = s->id;
            }
            // Track the overall least-loaded server as fallback
            if (bestSid == -1 || s->activeConnections < servers[bestSid]->activeConnections)
                bestSid = s->id;
        }
        // Prefer zone-match; fall back to global least-loaded
        if (bestZoneSid != -1) bestSid = bestZoneSid;

        if (bestSid == -1) return {false, -1, -1, "No Healthy Servers"};

        // Cache the assignment in the Splay Tree for next time
        sessionCache.insert(clientId, bestSid);

        int rid = reqIdCounter++;
        
        // SESSION PERSISTENCE (Splay-Tree Caching)
        // Facilitates sub-millisecond session affinity for repeat clients.
        // Complexity: O(1) Amortized Cache-Hit.
        servers[bestSid]->requestQueue.insert({now + slaMs, rid, clientId, priority});
        servers[bestSid]->activeConnections++;
        servers[bestSid]->totalRequests++;
        
        updateIndex(servers[bestSid]);
        return {true, bestSid, rid, "OK"};
    }

    // FAILOVER PROTOCOL: Distributed Queue Migration
    // Uses Binomial Heap extractMin() + insert() to redistribute requests,
    // and Skip List updateIndex() to maintain the sorted server index.
    // Complexity: O(k log n) where k = requests in dead queue.
    void killServer(int sid) {
        std::lock_guard<std::mutex> lock(lbMutex);
        if (sid < 0 || sid >= (int)servers.size()) return;
        
        servers[sid]->healthy = false;
        zoneDsu.killServer(sid);  // Mark dead in DSU
        serverIndex.remove(servers[sid]->activeConnections * 1000 + sid);
        
        // Collect healthy targets
        std::vector<Server*> healthyServers;
        for (auto s : servers) {
            if (s->healthy && s->id != sid) healthyServers.push_back(s);
        }
        
        if (healthyServers.empty()) {
            servers[sid]->activeConnections = 0;
            return;
        }

        // Extract each request from the dead server's Binomial Heap
        // and re-insert it into the least-loaded healthy server's heap.
        int redistributed = 0;
        while (!servers[sid]->requestQueue.empty()) {
            Request req = servers[sid]->requestQueue.extractMin();

            // Find the current least-loaded healthy server
            Server* target = healthyServers[0];
            for (auto s : healthyServers) {
                if (s->activeConnections < target->activeConnections) target = s;
            }

            // Insert into target's Binomial Heap
            target->requestQueue.insert(req);
            target->activeConnections++;
            updateIndex(target);  // Update Skip List index
            redistributed++;
        }

        servers[sid]->activeConnections = 0;
    }

    void complete(int sid, int rid, int latency, bool failed) {
        std::lock_guard<std::mutex> lock(lbMutex);
        if (sid < 0 || sid >= (int)servers.size()) return;

        Server* s = servers[sid];
        if (!s->requestQueue.empty()) s->requestQueue.extractMin();
        
        s->activeConnections = std::max(0, s->activeConnections - 1);
        updateIndex(s);
        
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        int sec = std::chrono::duration_cast<std::chrono::seconds>(now).count() % 60;
        
        // SEGMENT TREE UPDATE (Unit 5)
        // Record current throughput for Range Max Queries in O(log n)
        s->peakTracker.set(sec, s->activeConnections);
        
        s->metrics->recordRequest(sec, latency, failed);

        if (failed) {
            if (s->metrics->errorRate() > 50.0) {
                s->circuitState = CircuitState::OPEN;
                s->lastOpenTime = getNowMs();
            }
        } else {
            s->circuitState = CircuitState::CLOSED;
        }
    }

    // UNIT 5 ANALYTICS: Get peak connections in a given time range
    // Segment Tree RMQ (Range Maximum Query) in O(log n)
    int getPeakLoad(int sid, int startSec, int endSec) {
        std::lock_guard<std::mutex> lock(lbMutex);
        if (sid < 0 || sid >= (int)servers.size()) return 0;
        return servers[sid]->peakTracker.getMaxRange(startSec, endSec);
    }

    void reviveServer(int sid) {
        std::lock_guard<std::mutex> lock(lbMutex);
        if (sid < 0 || sid >= (int)servers.size()) return;
        servers[sid]->healthy = true;
        servers[sid]->circuitState = CircuitState::CLOSED;
        zoneDsu.reviveServer(sid);  // Mark alive in DSU
        updateIndex(servers[sid]);

        // REBALANCE: Redistribute load from overloaded servers
        // using Binomial Heap extract/insert and Skip List index updates
        std::vector<Server*> allHealthy;
        int totalConn = 0;
        for (auto s : servers) {
            if (s->healthy) {
                allHealthy.push_back(s);
                totalConn += s->activeConnections;
            }
        }
        if (allHealthy.size() <= 1 || totalConn == 0) return;

        int avgConn = totalConn / (int)allHealthy.size();

        // Extract excess requests from overloaded servers via Binomial Heap
        // and re-insert them into underloaded servers via Binomial Heap
        for (auto donor : allHealthy) {
            while (donor->activeConnections > avgConn + 1 && !donor->requestQueue.empty()) {
                // Find the most underloaded server
                Server* receiver = nullptr;
                for (auto s : allHealthy) {
                    if (s->activeConnections < avgConn) {
                        if (!receiver || s->activeConnections < receiver->activeConnections)
                            receiver = s;
                    }
                }
                if (!receiver) break;

                // Binomial Heap extractMin from donor
                Request req = donor->requestQueue.extractMin();
                donor->activeConnections--;
                updateIndex(donor);

                // Binomial Heap insert into receiver
                receiver->requestQueue.insert(req);
                receiver->activeConnections++;
                updateIndex(receiver);
            }
        }
    }

    void killZone(const std::string& zone) {
        // DSU batch-kills all servers in this zone component
        zoneDsu.killZone(zone);
        // Then trigger per-server failover for queue migration
        for (auto s : servers) {
            if (s->zone == zone && s->healthy) killServer(s->id);
        }
    }

    void reviveZone(const std::string& zone) {
        // DSU batch-revives the entire zone component
        zoneDsu.reviveZone(zone);
        for (auto s : servers) {
            if (s->zone == zone && !s->healthy) reviveServer(s->id);
        }
    }

    void printStatus() {
        std::lock_guard<std::mutex> lock(lbMutex);
        std::cout << "\n--- Load Balancer Status ---\n";
        for (auto s : servers) {
            std::cout << "Server " << s->id << " [" << s->zone << "]: "
                      << (s->healthy ? "UP  " : "DOWN") << " | "
                      << "Conn: " << s->activeConnections << " | "
                      << "Err: " << (int)s->metrics->errorRate() << "% | "
                      << "Lat: " << (int)s->metrics->avgLatency() << "ms\n";
        }
        std::cout << "---------------------------\n";
    }

    int getConnections(int sid) {
        std::lock_guard<std::mutex> lock(lbMutex);
        if (sid < 0 || sid >= (int)servers.size()) return 0;
        return servers[sid]->activeConnections;
    }

    int getQueueSize(int sid) {
        std::lock_guard<std::mutex> lock(lbMutex);
        if (sid < 0 || sid >= (int)servers.size()) return 0;
        return servers[sid]->requestQueue.size();
    }

    float getErrorRate(int sid) {
        std::lock_guard<std::mutex> lock(lbMutex);
        if (sid < 0 || sid >= (int)servers.size()) return 0;
        return servers[sid]->metrics->errorRate();
    }

    int getLatency(int sid) {
        std::lock_guard<std::mutex> lock(lbMutex);
        if (sid < 0 || sid >= (int)servers.size()) return 0;
        return (int)servers[sid]->metrics->avgLatency();
    }

    const char* getCircuitState(int sid) {
        std::lock_guard<std::mutex> lock(lbMutex);
        if (sid < 0 || sid >= (int)servers.size()) return "DEAD";
        switch(servers[sid]->circuitState) {
            case CircuitState::CLOSED: return "CLOSED";
            case CircuitState::OPEN: return "OPEN";
            case CircuitState::HALF_OPEN: return "HALF_OPEN";
            default: return "UNKNOWN";
        }
    }
};
