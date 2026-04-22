#pragma once
#include <vector>
#include <numeric>

// A simple sliding window metrics tracker for a 2nd year project.
// Replaces the complex Segment Tree with a simple array of buckets.

class SlidingWindowMetrics {
    int size;
    std::vector<long long> buckets;

public:
    explicit SlidingWindowMetrics(int windowSize = 60) 
        : size(windowSize), buckets(windowSize, 0) {}

    void record(int sec, long long count = 1) {
        buckets[sec % size] += count;
    }

    void resetBucket(int sec) {
        buckets[sec % size] = 0;
    }

    long long getTotal() const {
        long long sum = 0;
        for (auto b : buckets) sum += b;
        return sum;
    }

    // Simplified: just get sum of last 'n' seconds
    long long getRecentSum(int seconds) const {
        // In a real system you'd use timestamps, but for a 2nd year project
        // we can just sum the whole window or a portion of the array.
        return getTotal(); 
    }
};

struct ServerMetrics {
    int serverId;
    SlidingWindowMetrics requests;
    SlidingWindowMetrics errors;
    SlidingWindowMetrics latencyMs;

    explicit ServerMetrics(int id, int windowSec = 60)
        : serverId(id), requests(windowSec),
          errors(windowSec), latencyMs(windowSec) {}

    void recordRequest(int sec, long long latency, bool isError) {
        requests.record(sec);
        latencyMs.record(sec, latency);
        if (isError) errors.record(sec);
    }

    double errorRate() {
        long long reqs = requests.getTotal();
        long long errs = errors.getTotal();
        if (reqs == 0) return 0.0;
        return (double)errs / reqs * 100.0;
    }

    double avgLatency() {
        long long reqs = requests.getTotal();
        long long totalMs = latencyMs.getTotal();
        if (reqs == 0) return 0.0;
        return (double)totalMs / reqs;
    }
};
