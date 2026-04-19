#pragma once
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <string>
#include <atomic>

// Token Bucket Rate Limiter — per client
// Each client gets a bucket of capacity `maxTokens`
// Tokens refill at `refillRate` tokens/second
// A request costs `cost` tokens (default 1)
// If bucket < cost  → request rejected (429)
// Thread-safe via per-bucket spinlock

struct TokenBucket {
    double   tokens;
    double   maxTokens;
    double   refillRate;       // tokens per second
    std::chrono::steady_clock::time_point lastRefill;
    std::mutex mtx;

    TokenBucket(double max, double rate)
        : tokens(max), maxTokens(max), refillRate(rate),
          lastRefill(std::chrono::steady_clock::now()) {}

    // Returns true if request is allowed, false if rate-limited
    bool consume(double cost = 1.0) {
        std::lock_guard<std::mutex> lk(mtx);
        auto now     = std::chrono::steady_clock::now();
        double secs  = std::chrono::duration<double>(now - lastRefill).count();
        tokens       = std::min(maxTokens, tokens + secs * refillRate);
        lastRefill   = now;

        if (tokens >= cost) {
            tokens -= cost;
            return true;   // allowed
        }
        return false;      // rate limited
    }

    double available() {
        std::lock_guard<std::mutex> lk(mtx);
        auto now    = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(now - lastRefill).count();
        return std::min(maxTokens, tokens + secs * refillRate);
    }
};

struct RateLimitResult {
    bool   allowed;
    double tokensRemaining;
    int    retryAfterMs;   // 0 if allowed
};

class RateLimiter {
    std::unordered_map<int, TokenBucket*> buckets;
    std::mutex mapMtx;

    double defaultMax;
    double defaultRate;

    // Global rate limiter for burst protection
    TokenBucket* globalBucket;

public:
    // maxTokens  = burst capacity (e.g. 100 requests burst)
    // refillRate = sustained rate (e.g. 20 req/sec)
    RateLimiter(double maxTokens = 100.0, double refillRate = 20.0)
        : defaultMax(maxTokens), defaultRate(refillRate) {
        globalBucket = new TokenBucket(maxTokens * 5, refillRate * 5);
    }

    ~RateLimiter() {
        for (auto& [k,v] : buckets) delete v;
        delete globalBucket;
    }

    // Check if clientId is allowed to make a request
    RateLimitResult check(int clientId, double cost = 1.0) {
        // global check first
        if (!globalBucket->consume(cost)) {
            return {false, globalBucket->available(), 50};
        }

        // per-client check
        TokenBucket* bucket = nullptr;
        {
            std::lock_guard<std::mutex> lk(mapMtx);
            auto it = buckets.find(clientId);
            if (it == buckets.end()) {
                buckets[clientId] = new TokenBucket(defaultMax, defaultRate);
                bucket = buckets[clientId];
            } else {
                bucket = it->second;
            }
        }

        bool ok = bucket->consume(cost);
        double remaining = bucket->available();

        if (!ok) {
            // estimate ms until 1 token refills
            int retryMs = (int)(1000.0 / defaultRate) + 1;
            return {false, remaining, retryMs};
        }
        return {true, remaining, 0};
    }

    // Override rate for a specific client (VIP / penalized)
    void setClientLimit(int clientId, double maxTokens, double rate) {
        std::lock_guard<std::mutex> lk(mapMtx);
        if (buckets.count(clientId)) delete buckets[clientId];
        buckets[clientId] = new TokenBucket(maxTokens, rate);
    }

    // Penalize abusive client — drain their bucket
    void penalize(int clientId) {
        std::lock_guard<std::mutex> lk(mapMtx);
        if (buckets.count(clientId)) {
            buckets[clientId]->tokens = 0;
        }
    }

    // Stats
    int activeClients() {
        std::lock_guard<std::mutex> lk(mapMtx);
        return (int)buckets.size();
    }

    double clientTokens(int clientId) {
        std::lock_guard<std::mutex> lk(mapMtx);
        auto it = buckets.find(clientId);
        if (it == buckets.end()) return defaultMax;
        return it->second->available();
    }
};
