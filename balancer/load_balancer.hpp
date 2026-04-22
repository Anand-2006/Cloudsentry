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
};

enum class CircuitState { CLOSED, OPEN, HALF_OPEN };

struct Server {
    int id;
    std::string zone;
    int activeConnections = 0;
    int totalRequests = 0;
    bool healthy = true;
    CircuitState circuitState = CircuitState::CLOSED;
    
    // Standard STL structures instead of custom trees/heaps
    std::priority_queue<Request, std::vector<Request>, std::greater<Request>> requestQueue;
    ServerMetrics* metrics;

    Server(int id, const std::string& zone) : id(id), zone(zone) {
        metrics = new ServerMetrics(id);
    }
    ~Server() { delete metrics; }
};

class LoadBalancer {
    std::vector<Server*> servers;
    SimpleSkipList serverIndex; // Helps pick server with min connections
    RateLimiter rateLimiter;
    std::mutex lbMutex; // One big mutex - simple and effective for 2nd year
    
    int reqIdCounter = 0;
    bool running = true;
    std::thread healthMonitor;

public:
    LoadBalancer(int n, double rlMax = 50.0, double rlRate = 15.0) 
        : rateLimiter(rlMax, rlRate) {
        
        std::vector<std::string> zones = {"us-east-1a", "us-east-1b", "us-west-1a"};
        for (int i = 0; i < n; i++) {
            servers.push_back(new Server(i, zones[i % 3]));
            serverIndex.insert(i, 0); // initial load 0
        }
    }

    ~LoadBalancer() {
        for (auto s : servers) delete s;
    }

    // Main Routing Logic
    struct RouteResult {
        bool accepted;
        int serverId;
        int requestId;
        std::string reason;
    };

    RouteResult route(int clientId, int priority, int slaMs = 2000) {
        std::lock_guard<std::mutex> lock(lbMutex);

        // 1. Check Rate Limiter
        auto rl = rateLimiter.check(clientId);
        if (!rl.allowed) return {false, -1, -1, "Rate Limited"};

        // 2. Pick best server (Simplification: just search for min connections)
        int bestSid = -1;
        int minConn = INT_MAX;
        for (auto s : servers) {
            if (s->healthy && s->circuitState != CircuitState::OPEN) {
                if (s->activeConnections < minConn) {
                    minConn = s->activeConnections;
                    bestSid = s->id;
                }
            }
        }

        if (bestSid == -1) return {false, -1, -1, "No Healthy Servers"};

        // 3. Enqueue Request
        int rid = reqIdCounter++;
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        
        servers[bestSid]->requestQueue.push({nowMs + slaMs, rid, clientId, priority});
        servers[bestSid]->activeConnections++;
        servers[bestSid]->totalRequests++;

        return {true, bestSid, rid, "OK"};
    }

    void complete(int sid, int rid, int latency, bool failed) {
        std::lock_guard<std::mutex> lock(lbMutex);
        if (sid < 0 || sid >= (int)servers.size()) return;

        Server* s = servers[sid];
        s->activeConnections = std::max(0, s->activeConnections - 1);
        
        // Record metrics
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        int sec = std::chrono::duration_cast<std::chrono::seconds>(now).count() % 60;
        s->metrics->recordRequest(sec, latency, failed);

        // Simple Circuit Breaker logic
        if (failed) {
            if (s->metrics->errorRate() > 50.0) s->circuitState = CircuitState::OPEN;
        } else {
            if (s->circuitState == CircuitState::OPEN) s->circuitState = CircuitState::HALF_OPEN;
            else s->circuitState = CircuitState::CLOSED;
        }
    }

    // Failover: Just move requests to another server
    void killServer(int sid) {
        std::lock_guard<std::mutex> lock(lbMutex);
        if (sid < 0 || sid >= (int)servers.size()) return;
        
        servers[sid]->healthy = false;
        std::cout << "[System] Server " << sid << " DIED. Moving requests...\n";

        // Find another healthy server
        int backupSid = -1;
        for (auto s : servers) {
            if (s->healthy && s->id != sid) {
                backupSid = s->id;
                break;
            }
        }

        if (backupSid != -1) {
            while (!servers[sid]->requestQueue.empty()) {
                servers[backupSid]->requestQueue.push(servers[sid]->requestQueue.top());
                servers[sid]->requestQueue.pop();
                servers[backupSid]->activeConnections++;
            }
        }
    }

    void reviveServer(int sid) {
        std::lock_guard<std::mutex> lock(lbMutex);
        if (sid < 0 || sid >= (int)servers.size()) return;
        servers[sid]->healthy = true;
        servers[sid]->circuitState = CircuitState::CLOSED;
    }

    void killZone(const std::string& zone) {
        for (auto s : servers) {
            if (s->zone == zone) killServer(s->id);
        }
    }

    void reviveZone(const std::string& zone) {
        for (auto s : servers) {
            if (s->zone == zone) reviveServer(s->id);
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
};
