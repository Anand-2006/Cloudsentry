#include <iostream>
#include <thread>
#include <vector>
#include <random>
#include <chrono>
#include <math>
#include "balancer/load_balancer.hpp"

// Simple simulation for a 2nd year project
void simulateTraffic(LoadBalancer& lb, int numRequests) {
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> latDist(10, 150);
    std::uniform_int_distribution<int> failDist(0, 100);

    for (int i = 0; i < numRequests; i++) {
        // 1. Try to route
        auto result = lb.route(1, 10);
        
        if (result.accepted) {
            // 2. Simulate some work
            int latency = latDist(rng);
            bool failed = failDist(rng) > 95; // 5% chance of failure
            
            // 3. Complete request
            lb.complete(result.serverId, result.requestId, latency, failed);
        } else {
            std::cout << "Request " << i << " Rejected: " << result.reason << "\n";
        }

        // Small delay between requests
        if (i % 20 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int main() {
    std::cout << "==========================================\n";
    std::cout << "   CloudSentry v2 (Simplified Version)    \n";
    std::cout << "   2nd Year Data Structures Project       \n";
    std::cout << "==========================================\n";

    // Initialize with 6 servers
    LoadBalancer lb(6);
    lb.printStatus();

    std::cout << "\n[Step 1] Simulating normal traffic...\n";
    simulateTraffic(lb, 100);
    lb.printStatus();

    std::cout << "\n[Step 2] Killing Zone us-east-1a (Failover demonstration)...\n";
    lb.killZone("us-east-1a");
    lb.printStatus();

    std::cout << "\n[Step 3] Simulating traffic during failure...\n";
    simulateTraffic(lb, 100);
    lb.printStatus();

    std::cout << "\n[Step 4] Reviving servers...\n";
    lb.reviveZone("us-east-1a");
    lb.printStatus();

    std::cout << "\n[Final] Simulation Complete.\n";
    return 0;
}
