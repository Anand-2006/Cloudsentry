#pragma once
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <iostream>
#include <functional>
#include <unordered_map>
#include <optional>
#include <climits>
#include <sstream>
#include "../core/skip_list/lock_free.hpp"
#include "../core/binomial_heap/binomial_heap.hpp"
#include "../core/segment_tree/metrics_segtree.hpp"
#include "../core/union_find/zone_dsu.hpp"
#include "../core/rb_tree/request_rbtree.hpp"
#include "rate_limiter.hpp"

enum class CircuitState { CLOSED, OPEN, HALF_OPEN };

inline std::string circuitStateStr(CircuitState s) {
    switch(s) {
        case CircuitState::CLOSED:    return "CLOSED";
        case CircuitState::OPEN:      return "OPEN";
        case CircuitState::HALF_OPEN: return "HALF_OPEN";
    }
    return "CLOSED";
}

struct Server {
    int id;
    std::string zone;
    std::atomic<int>  activeConnections{0};
    std::atomic<int>  totalRequests{0};
    std::atomic<bool> healthy{true};
    CircuitState circuitState{CircuitState::CLOSED};
    std::mutex   circuitMtx;
    int          circuitFailCount{0};
    std::chrono::steady_clock::time_point lastFailTime;

    // RB-Tree: deadline-ordered pending request queue
    RequestRBTree requestQueue;
    std::mutex    queueMtx;

    // Binomial Heap: used during failover merge only
    BinomialHeap  failoverHeap;
    std::mutex    heapMtx;

    Server(int id, const std::string& zone) : id(id), zone(zone) {}
};

struct RouteResult {
    bool   accepted;
    int    serverId;
    int    requestId;
    std::string reason;   // rejection reason if !accepted
};

class LoadBalancer {
    int numServers;
    std::vector<Server*> servers;
    LockFreeSkipList     serverIndex;   // (load*1000+sid) -> sid
    ZoneDSU              zoneDsu;
    std::vector<ServerMetrics*> metrics;
    RateLimiter          rateLimiter;

    std::atomic<bool>    running{true};
    std::thread          healthMonitor;
    std::atomic<int>     reqIdCounter{0};

    static const int CIRCUIT_THRESHOLD  = 5;
    static const int CIRCUIT_TIMEOUT_MS = 5000;
    static const int DEFAULT_DEADLINE_MS = 2000;  // 2s SLA

    // ── helpers ─────────────────────────────────────────────────────────────
    long long nowMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    int currentSecond() {
        using namespace std::chrono;
        return (int)(duration_cast<seconds>(steady_clock::now().time_since_epoch()).count() % 60);
    }

    int loadScore(int sid) {
        return servers[sid]->activeConnections.load(std::memory_order_relaxed);
    }

    void refreshIndex(int sid) {
        // remove any stale entry, reinsert with current load
        int score = loadScore(sid) * 1000 + sid;
        serverIndex.remove(score);
        serverIndex.insert(score, sid);
    }

    // ── Circuit Breaker ──────────────────────────────────────────────────────
    bool checkCircuit(int sid) {
        Server* s = servers[sid];
        std::lock_guard<std::mutex> lk(s->circuitMtx);
        if (s->circuitState == CircuitState::OPEN) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - s->lastFailTime).count();
            if (elapsed > CIRCUIT_TIMEOUT_MS) {
                s->circuitState = CircuitState::HALF_OPEN;
                return true;
            }
            return false;
        }
        return true;
    }

    void onFailure(int sid) {
        Server* s = servers[sid];
        std::lock_guard<std::mutex> lk(s->circuitMtx);
        s->circuitFailCount++;
        if (s->circuitFailCount >= CIRCUIT_THRESHOLD) {
            s->circuitState = CircuitState::OPEN;
            s->lastFailTime = std::chrono::steady_clock::now();
            std::cout << "[Circuit] Server " << sid << " OPEN\n";
        }
    }

    void onSuccess(int sid) {
        Server* s = servers[sid];
        std::lock_guard<std::mutex> lk(s->circuitMtx);
        s->circuitFailCount = 0;
        if (s->circuitState == CircuitState::HALF_OPEN) {
            s->circuitState = CircuitState::CLOSED;
            std::cout << "[Circuit] Server " << sid << " CLOSED (recovered)\n";
        }
    }

    // ── Failover: Binomial Heap merge O(log n) ───────────────────────────────
    void triggerFailover(int deadSid) {
        // Drain RB-Tree into a Binomial Heap for dead server
        Server* dead = servers[deadSid];
        BinomialHeap orphans;
        {
            std::lock_guard<std::mutex> lk(dead->queueMtx);
            while (!dead->requestQueue.empty()) {
                auto req = dead->requestQueue.extractMin();
                if (req) orphans.insert((int)req->deadlineMs, req->requestId);
            }
        }
        if (orphans.empty()) return;

        // Pick least-loaded alive server as target
        auto alive = zoneDsu.aliveServers();
        int targetSid = -1, minLoad = INT_MAX;
        for (int s : alive) {
            if (s == deadSid || !servers[s]->healthy.load()) continue;
            int load = servers[s]->activeConnections.load();
            if (load < minLoad) { minLoad = load; targetSid = s; }
        }
        if (targetSid == -1) return;

        // O(log n) Binomial Heap merge into target's failover heap
        {
            std::lock_guard<std::mutex> lk(servers[targetSid]->heapMtx);
            servers[targetSid]->failoverHeap.merge(orphans);
        }
        std::cout << "[Failover] Binomial Heap merge: server " << deadSid
                  << " -> server " << targetSid << "  O(log n)\n";
    }

    // ── Health Monitor thread ────────────────────────────────────────────────
    void healthLoop() {
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            for (auto* s : servers) {
                bool prevHealthy = s->healthy.load();
                int  sec  = currentSecond();
                int  from = (sec - 10 + 60) % 60;
                double errRate  = metrics[s->id]->errorRate(from, sec);
                bool nowHealthy = errRate < 50.0 && zoneDsu.isAlive(s->id);

                if (prevHealthy && !nowHealthy) {
                    s->healthy.store(false);
                    zoneDsu.killServer(s->id);
                    triggerFailover(s->id);
                    std::cout << "[Health] Server " << s->id << " UNHEALTHY (errRate="
                              << errRate << "%)\n";
                } else if (!prevHealthy && nowHealthy) {
                    s->healthy.store(true);
                    zoneDsu.reviveServer(s->id);
                    std::cout << "[Health] Server " << s->id << " RECOVERED\n";
                }
            }
        }
    }

public:
    explicit LoadBalancer(int n,
                          double rlMax  = 100.0,
                          double rlRate = 20.0)
        : numServers(n), zoneDsu(n), rateLimiter(rlMax, rlRate) {

        std::vector<std::string> zones = {"us-east-1a", "us-east-1b", "us-west-1a"};
        for (int i = 0; i < n; i++) {
            std::string zone = zones[i % zones.size()];
            servers.push_back(new Server(i, zone));
            zoneDsu.assignZone(i, zone);
            metrics.push_back(new ServerMetrics(i));
            serverIndex.insert(i, i);
        }
        for (int i = 0; i < n; i++)
            for (int j = i+1; j < n; j++)
                if (servers[i]->zone == servers[j]->zone)
                    zoneDsu.unionServers(i, j);

        healthMonitor = std::thread(&LoadBalancer::healthLoop, this);
    }

    ~LoadBalancer() {
        running = false;
        if (healthMonitor.joinable()) healthMonitor.join();
        for (auto* s : servers) delete s;
        for (auto* m : metrics)  delete m;
    }

    // ── Main route entry point ───────────────────────────────────────────────
    RouteResult route(int clientId, int priority = 50, int slaMs = DEFAULT_DEADLINE_MS) {
        // 1. Rate limit check
        auto rl = rateLimiter.check(clientId);
        if (!rl.allowed)
            return {false, -1, -1, "rate_limited (tokens=" +
                    std::to_string((int)rl.tokensRemaining) +
                    ", retry=" + std::to_string(rl.retryAfterMs) + "ms)"};

        // 2. Pick best alive server — least connections, circuit CLOSED
        auto alive = zoneDsu.aliveServers();
        if (alive.empty())
            return {false, -1, -1, "no_alive_servers"};

        int sid = -1, minLoad = INT_MAX;
        for (int s : alive) {
            if (!servers[s]->healthy.load()) continue;
            if (!checkCircuit(s))            continue;
            int load = servers[s]->activeConnections.load(std::memory_order_relaxed);
            if (load < minLoad) { minLoad = load; sid = s; }
        }
        if (sid == -1)
            return {false, -1, -1, "all_circuits_open"};

        // 3. Build request and enqueue in RB-Tree (EDF scheduling)
        int reqId = reqIdCounter.fetch_add(1, std::memory_order_relaxed);
        Request req {
            nowMs() + slaMs,   // deadlineMs
            reqId,
            clientId,
            priority,
            nowMs()
        };

        {
            std::lock_guard<std::mutex> lk(servers[sid]->queueMtx);
            servers[sid]->requestQueue.insert(req);
        }

        // 4. Update load index
        servers[sid]->activeConnections.fetch_add(1);
        servers[sid]->totalRequests.fetch_add(1);
        refreshIndex(sid);

        return {true, sid, reqId, "ok"};
    }

    // ── Called when request finishes ─────────────────────────────────────────
    void complete(int sid, int reqId, long long latencyMs, bool failed = false) {
        // remove from RB-Tree
        {
            std::lock_guard<std::mutex> lk(servers[sid]->queueMtx);
            servers[sid]->requestQueue.removeById(reqId);
        }

        servers[sid]->activeConnections.fetch_add(-1);
        refreshIndex(sid);

        int sec = currentSecond();
        metrics[sid]->recordRequest(sec, latencyMs, failed);

        if (failed) onFailure(sid);
        else        onSuccess(sid);
    }

    // ── Reprioritize a pending request ───────────────────────────────────────
    bool reprioritize(int sid, int reqId, long long newDeadlineMs) {
        std::lock_guard<std::mutex> lk(servers[sid]->queueMtx);
        return servers[sid]->requestQueue.updateDeadline(reqId, newDeadlineMs);
    }

    // ── Cancel a pending request ─────────────────────────────────────────────
    bool cancel(int sid, int reqId) {
        std::lock_guard<std::mutex> lk(servers[sid]->queueMtx);
        bool removed = servers[sid]->requestQueue.removeById(reqId);
        if (removed) {
            servers[sid]->activeConnections.fetch_add(-1);
            refreshIndex(sid);
        }
        return removed;
    }

    // ── Zone / Server controls ───────────────────────────────────────────────
    void killZone(const std::string& zone) {
        zoneDsu.killZone(zone);
        std::cout << "[Zone] " << zone << " KILLED\n";
        for (int sid : zoneDsu.serversInZone(zone)) {
            servers[sid]->healthy.store(false);
            triggerFailover(sid);
        }
    }

    void reviveZone(const std::string& zone) {
        zoneDsu.reviveZone(zone);
        std::cout << "[Zone] " << zone << " REVIVED\n";
        for (int sid : zoneDsu.serversInZone(zone)) {
            servers[sid]->healthy.store(true);
            std::lock_guard<std::mutex> lk(servers[sid]->circuitMtx);
            servers[sid]->circuitState    = CircuitState::CLOSED;
            servers[sid]->circuitFailCount = 0;
        }
    }

    void killServer(int sid) {
        servers[sid]->healthy.store(false);
        zoneDsu.killServer(sid);
        triggerFailover(sid);
        std::cout << "[Server] " << sid << " KILLED\n";
    }

    void reviveServer(int sid) {
        servers[sid]->healthy.store(true);
        zoneDsu.reviveServer(sid);
        std::lock_guard<std::mutex> lk(servers[sid]->circuitMtx);
        servers[sid]->circuitState     = CircuitState::CLOSED;
        servers[sid]->circuitFailCount = 0;
        std::cout << "[Server] " << sid << " REVIVED\n";
    }

    void setClientRateLimit(int clientId, double max, double rate) {
        rateLimiter.setClientLimit(clientId, max, rate);
    }

    // ── Getters for API / dashboard ──────────────────────────────────────────
    int    getNumServers()     const { return numServers; }
    int    getConnections(int sid)   { return servers[sid]->activeConnections.load(); }
    int    getTotalRequests(int sid) { return servers[sid]->totalRequests.load(); }
    bool   isHealthy(int sid)        { return servers[sid]->healthy.load(); }
    int    queueDepth(int sid)       { return servers[sid]->requestQueue.size(); }
    std::string getZone(int sid)     { return servers[sid]->zone; }
    CircuitState getCircuit(int sid) { return servers[sid]->circuitState; }

    double getErrorRate(int sid) {
        int sec = currentSecond(), from = (sec - 10 + 60) % 60;
        return metrics[sid]->errorRate(from, sec);
    }
    double getAvgLatency(int sid) {
        int sec = currentSecond(), from = (sec - 10 + 60) % 60;
        return metrics[sid]->avgLatency(from, sec);
    }
    double clientTokens(int clientId) {
        return rateLimiter.clientTokens(clientId);
    }

    void printStatus() {
        std::cout << "\n┌─────────────────────────────────────────────────────────────┐\n";
        std::cout <<   "│ Server Status                                               │\n";
        std::cout <<   "├────┬────────────┬──────┬───────┬──────────┬────────┬───────┤\n";
        std::cout <<   "│ ID │ Zone       │ Conn │ Queue │ ErrRate  │ Lat ms │Circuit│\n";
        std::cout <<   "├────┼────────────┼──────┼───────┼──────────┼────────┼───────┤\n";
        for (auto* s : servers) {
            printf("│ %2d │ %-10s │  %3d │   %3d │ %6.2f%% │ %5.1fms │ %-5s │\n",
                s->id,
                s->zone.substr(0, 10).c_str(),
                s->activeConnections.load(),
                s->requestQueue.size(),
                getErrorRate(s->id),
                getAvgLatency(s->id),
                s->healthy.load()
                    ? circuitStateStr(s->circuitState).substr(0,5).c_str()
                    : "DEAD ");
        }
        std::cout << "└────┴────────────┴──────┴───────┴──────────┴────────┴───────┘\n\n";
    }
};
