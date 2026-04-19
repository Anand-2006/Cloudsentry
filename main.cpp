#include <iostream>
#include <thread>
#include <vector>
#include <random>
#include <atomic>
#include <chrono>
#include <iomanip>
#include "balancer/load_balancer.hpp"

using namespace std::chrono;

// ── Shared counters ──────────────────────────────────────────────────────────
std::atomic<long long> totalRouted{0};
std::atomic<long long> totalFailed{0};
std::atomic<long long> totalRateLimited{0};
std::atomic<long long> totalCancelled{0};

// ── Worker thread: fires requests, simulates latency, calls complete() ───────
void clientWorker(LoadBalancer& lb, int clientId,
                  int numRequests, int slaMs = 2000,
                  bool abusive = false) {
    std::mt19937 rng(clientId * 999983);
    std::uniform_int_distribution<int> latDist(5, 300);
    std::uniform_int_distribution<int> failDist(0, 99);
    std::uniform_int_distribution<int> priDist(0, 99);
    std::uniform_int_distribution<int> cancelDist(0, 9);

    for (int i = 0; i < numRequests; i++) {
        int  priority = priDist(rng);
        auto result   = lb.route(clientId, priority, slaMs);

        if (!result.accepted) {
            if (result.reason.find("rate_limited") != std::string::npos)
                totalRateLimited++;
            else
                totalFailed++;
            if (!abusive)
                std::this_thread::sleep_for(microseconds(500));
            continue;
        }

        long long lat  = latDist(rng);
        bool      fail = failDist(rng) < 5;   // 5% base failure rate

        // occasionally cancel a request mid-flight
        if (cancelDist(rng) == 0) {
            lb.cancel(result.serverId, result.requestId);
            totalCancelled++;
            continue;
        }

        std::this_thread::sleep_for(microseconds(lat * 80));
        lb.complete(result.serverId, result.requestId, lat, fail);
        totalRouted++;
    }
}

// ── Print phase header ────────────────────────────────────────────────────────
void phase(const std::string& title) {
    std::cout << "\n\033[1;36m=== " << title << " ===\033[0m\n";
}

void printCounters() {
    std::cout << "  Routed:       " << totalRouted       << "\n"
              << "  Failed:       " << totalFailed       << "\n"
              << "  Rate-limited: " << totalRateLimited  << "\n"
              << "  Cancelled:    " << totalCancelled    << "\n";
}

// ── Run a batch of client threads ────────────────────────────────────────────
void runBatch(LoadBalancer& lb, int numClients, int reqsEach,
              int slaMs = 2000, bool abusive = false) {
    std::vector<std::thread> threads;
    for (int i = 0; i < numClients; i++)
        threads.emplace_back(clientWorker, std::ref(lb),
                             i + 1, reqsEach, slaMs, abusive);
    for (auto& t : threads) t.join();
}

int main() {
    std::cout << "\n"
              << " ██████╗██╗      ██████╗ ██╗   ██╗██████╗\n"
              << "██╔════╝██║     ██╔═══██╗██║   ██║██╔══██╗\n"
              << "██║     ██║     ██║   ██║██║   ██║██║  ██║\n"
              << "██║     ██║     ██║   ██║██║   ██║██║  ██║\n"
              << "╚██████╗███████╗╚██████╔╝╚██████╔╝██████╔╝\n"
              << " ╚═════╝╚══════╝ ╚═════╝  ╚═════╝ ╚═════╝\n"
              << "   CloudSentry v2 — Full DS Integration\n\n";

    // LB: 9 servers, rate limit 50 tokens burst / 15 req/s refill
    LoadBalancer lb(9, 50.0, 15.0);

    std::cout << "[Init] 9 servers | 3 zones | RB-Tree queues | Binomial failover\n"
              << "       Lock-Free Skip List routing | Segment Tree metrics | DSU zones\n"
              << "       Token Bucket rate limiter (50 burst / 15 rps)\n";
    lb.printStatus();

    // ── Phase 1: Normal traffic ──────────────────────────────────────────────
    phase("Phase 1 — Normal Traffic (8 clients × 60 requests)");
    runBatch(lb, 8, 60);
    printCounters();
    lb.printStatus();

    // ── Phase 2: Rate limiting ───────────────────────────────────────────────
    phase("Phase 2 — Rate Limit Test (3 abusive clients hammering 200 requests)");
    long long beforeRL = totalRateLimited.load();
    runBatch(lb, 3, 200, 2000, true /*abusive: no backoff*/);
    std::cout << "  Rate-limited this phase: " << (totalRateLimited - beforeRL) << "\n";
    lb.printStatus();

    // ── Phase 3: Priority + cancel test ─────────────────────────────────────
    phase("Phase 3 — Priority Routing + Cancellations (6 clients × 50 requests)");
    long long beforeCancel = totalCancelled.load();
    runBatch(lb, 6, 50, 1000);
    std::cout << "  Cancelled this phase: " << (totalCancelled - beforeCancel) << "\n";
    lb.printStatus();

    // ── Phase 4: Zone failure + Binomial Heap failover ───────────────────────
    phase("Phase 4 — Zone Failure: us-east-1a (3 servers down)");
    lb.killZone("us-east-1a");
    std::this_thread::sleep_for(milliseconds(200));
    lb.printStatus();

    phase("Phase 4b — Traffic under zone failure");
    long long beforeFail = totalRouted.load();
    runBatch(lb, 8, 60);
    std::cout << "  Routed under failure: " << (totalRouted - beforeFail) << "\n";
    lb.printStatus();

    // ── Phase 5: Zone recovery ───────────────────────────────────────────────
    phase("Phase 5 — Zone Recovery: us-east-1a back online");
    lb.reviveZone("us-east-1a");
    std::this_thread::sleep_for(milliseconds(200));
    lb.printStatus();

    // ── Phase 6: Single server kill + circuit breaker ────────────────────────
    phase("Phase 6 — Single Server Kill + Circuit Breaker");
    lb.killServer(4);
    std::this_thread::sleep_for(milliseconds(100));
    runBatch(lb, 6, 40);
    lb.reviveServer(4);
    lb.printStatus();

    // ── Phase 7: Reprioritize test ───────────────────────────────────────────
    phase("Phase 7 — Reprioritize (RB-Tree updateDeadline)");
    // route 1 request and reprioritize it before it completes
    auto r = lb.route(999, 99, 5000);
    if (r.accepted) {
        bool ok = lb.reprioritize(r.serverId, r.requestId, 1); // push to front
        std::cout << "  Reprioritized req " << r.requestId
                  << " on server " << r.serverId
                  << ": " << (ok ? "success" : "not found") << "\n";
        lb.complete(r.serverId, r.requestId, 10, false);
        totalRouted++;
    }

    // ── Final stats ──────────────────────────────────────────────────────────
    phase("Final Summary");
    printCounters();
    lb.printStatus();

    std::cout << "\033[1;32m✓ Simulation complete.\033[0m\n\n";
    return 0;
}
