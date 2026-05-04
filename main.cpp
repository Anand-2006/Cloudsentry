#include <iostream>
#include <thread>
#include <vector>
#include <random>
#include <chrono>
#include <cmath>
#include "balancer/load_balancer.hpp"

void simulateTraffic(LoadBalancer& lb, int numRequests) {
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> latDist(10, 150);
    std::uniform_int_distribution<int> failDist(0, 100);
    std::uniform_int_distribution<int> clientDist(1, 50);   // 50 distinct clients
    std::uniform_int_distribution<int> priorityDist(1, 10);

    // Diverse URL paths so the Trie is exercised
    std::vector<std::string> paths = {"/api/v1/users", "/api/v2/orders", "/static/logo.png", "/api/v1/health", "/docs/readme"};
    std::uniform_int_distribution<int> pathDist(0, (int)paths.size() - 1);

    int accepted = 0, rejected = 0;

    for (int i = 0; i < numRequests; i++) {
        int clientId = clientDist(rng);
        int priority = priorityDist(rng);
        const std::string& path = paths[pathDist(rng)];

        auto result = lb.route(clientId, priority, path);
        
        if (result.accepted) {
            accepted++;
            int latency = latDist(rng);
            bool failed = failDist(rng) > 95; // 5% chance of failure
            lb.complete(result.serverId, latency, failed);
        } else {
            rejected++;
        }

        // Small delay between requests for rate limiter refill
        if (i % 20 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "  -> Accepted: " << accepted << " | Rejected: " << rejected << "\n";
}

int main() {
    std::cout << "==========================================\n";
    std::cout << "   CloudSentry v2 — Full DS Simulation    \n";
    std::cout << "   2nd Year Data Structures Project       \n";
    std::cout << "==========================================\n";

    // Initialize with 6 servers (higher rate-limit so simulation isn't starved)
    LoadBalancer lb(6, 500.0, 100.0);
    lb.printStatus();

    std::cout << "\n[Step 1] Simulating normal traffic (200 requests, 50 clients)...\n";
    simulateTraffic(lb, 200);
    lb.printStatus();

    std::cout << "\n[Step 2] Killing Zone us-east-1a (Failover demonstration)...\n";
    lb.killZone("us-east-1a");
    lb.printStatus();

    std::cout << "\n[Step 3] Simulating traffic during failure...\n";
    simulateTraffic(lb, 200);
    lb.printStatus();

    std::cout << "\n[Step 4] Reviving Zone us-east-1a...\n";
    lb.reviveZone("us-east-1a");
    lb.printStatus();

    std::cout << "\n[Step 5] Post-recovery traffic...\n";
    simulateTraffic(lb, 200);
    lb.printStatus();

    // Demonstrate Segment Tree analytics
    std::cout << "\n[Analytics] Peak load per server (last 60s window):\n";
    for (int sid = 0; sid < 6; sid++) {
        int peak = lb.getPeakLoad(sid, 0, 59);
        std::cout << "  Server " << sid << ": peak connections = " << peak << "\n";
    }

    std::cout << "\n[Final] Simulation Complete.\n";
    return 0;
}
