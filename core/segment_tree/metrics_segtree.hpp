#pragma once
#include <vector>
#include <algorithm>
#include <mutex>
#include <atomic>
#include <chrono>

// Thread-safe segment tree for sliding window metrics
// Each leaf = 1 second bucket
// Supports: range sum, range max, point update
// Used for: request rate, error rate, latency tracking

struct SegNode {
    long long sum;
    long long maxVal;
    long long lazy; // lazy addition
};

class MetricsSegTree {
    int n;
    std::vector<SegNode> tree;
    mutable std::mutex mtx;

    void build(int node, int l, int r) {
        tree[node] = {0, 0, 0};
        if (l == r) return;
        int mid = (l + r) / 2;
        build(2*node, l, mid);
        build(2*node+1, mid+1, r);
    }

    void pushDown(int node) {
        if (tree[node].lazy) {
            for (int child : {2*node, 2*node+1}) {
                tree[child].sum   += tree[node].lazy;
                tree[child].maxVal += tree[node].lazy;
                tree[child].lazy  += tree[node].lazy;
            }
            tree[node].lazy = 0;
        }
    }

    void update(int node, int l, int r, int pos, long long val) {
        if (l == r) {
            tree[node].sum    += val;
            tree[node].maxVal  = tree[node].sum;
            return;
        }
        pushDown(node);
        int mid = (l + r) / 2;
        if (pos <= mid) update(2*node, l, mid, pos, val);
        else            update(2*node+1, mid+1, r, pos, val);
        tree[node].sum    = tree[2*node].sum    + tree[2*node+1].sum;
        tree[node].maxVal = std::max(tree[2*node].maxVal, tree[2*node+1].maxVal);
    }

    void rangeAdd(int node, int l, int r, int ql, int qr, long long val) {
        if (qr < l || r < ql) return;
        if (ql <= l && r <= qr) {
            tree[node].sum    += val * (r - l + 1);
            tree[node].maxVal += val;
            tree[node].lazy   += val;
            return;
        }
        pushDown(node);
        int mid = (l + r) / 2;
        rangeAdd(2*node, l, mid, ql, qr, val);
        rangeAdd(2*node+1, mid+1, r, ql, qr, val);
        tree[node].sum    = tree[2*node].sum    + tree[2*node+1].sum;
        tree[node].maxVal = std::max(tree[2*node].maxVal, tree[2*node+1].maxVal);
    }

    long long querySum(int node, int l, int r, int ql, int qr) {
        if (qr < l || r < ql) return 0;
        if (ql <= l && r <= qr) return tree[node].sum;
        pushDown(node);
        int mid = (l + r) / 2;
        return querySum(2*node, l, mid, ql, qr)
             + querySum(2*node+1, mid+1, r, ql, qr);
    }

    long long queryMax(int node, int l, int r, int ql, int qr) {
        if (qr < l || r < ql) return 0;
        if (ql <= l && r <= qr) return tree[node].maxVal;
        pushDown(node);
        int mid = (l + r) / 2;
        return std::max(queryMax(2*node, l, mid, ql, qr),
                        queryMax(2*node+1, mid+1, r, ql, qr));
    }

    void pointSet(int node, int l, int r, int pos, long long val) {
        if (l == r) {
            tree[node].sum    = val;
            tree[node].maxVal = val;
            tree[node].lazy   = 0;
            return;
        }
        pushDown(node);
        int mid = (l + r) / 2;
        if (pos <= mid) pointSet(2*node, l, mid, pos, val);
        else            pointSet(2*node+1, mid+1, r, pos, val);
        tree[node].sum    = tree[2*node].sum    + tree[2*node+1].sum;
        tree[node].maxVal = std::max(tree[2*node].maxVal, tree[2*node+1].maxVal);
    }

public:
    // windowSize = seconds to track (e.g. 60 for 1-minute window)
    explicit MetricsSegTree(int windowSize) : n(windowSize), tree(4 * windowSize) {
        build(1, 0, n - 1);
    }

    // Increment bucket at second `sec` by `count`
    void record(int sec, long long count = 1) {
        std::lock_guard<std::mutex> lk(mtx);
        update(1, 0, n-1, sec % n, count);
    }

    // Reset a bucket (called when window slides past it)
    void resetBucket(int sec) {
        std::lock_guard<std::mutex> lk(mtx);
        pointSet(1, 0, n-1, sec % n, 0);
    }

    // Total ops in [from, to] seconds (inclusive, 0-indexed)
    long long rangeSum(int from, int to) {
        std::lock_guard<std::mutex> lk(mtx);
        return querySum(1, 0, n-1, from % n, to % n);
    }

    // Peak ops in any single second within [from, to]
    long long rangePeak(int from, int to) {
        std::lock_guard<std::mutex> lk(mtx);
        return queryMax(1, 0, n-1, from % n, to % n);
    }

    // Bulk add to a range of buckets (e.g. simulate load spike)
    void bulkAdd(int from, int to, long long val) {
        std::lock_guard<std::mutex> lk(mtx);
        rangeAdd(1, 0, n-1, from % n, to % n, val);
    }

    int windowSize() const { return n; }
};


// ─── Per-Server Metrics Tracker ────────────────────────────────────────────
struct ServerMetrics {
    int serverId;
    MetricsSegTree requests;  // requests per second
    MetricsSegTree errors;    // errors per second
    MetricsSegTree latencyMs; // summed latency per second

    explicit ServerMetrics(int id, int windowSec = 60)
        : serverId(id), requests(windowSec),
          errors(windowSec), latencyMs(windowSec) {}

    void recordRequest(int sec, long long latency, bool isError) {
        requests.record(sec);
        latencyMs.record(sec, latency);
        if (isError) errors.record(sec);
    }

    double errorRate(int fromSec, int toSec) {
        long long reqs = requests.rangeSum(fromSec, toSec);
        long long errs = errors.rangeSum(fromSec, toSec);
        if (reqs == 0) return 0.0;
        return static_cast<double>(errs) / reqs * 100.0;
    }

    double avgLatency(int fromSec, int toSec) {
        long long reqs   = requests.rangeSum(fromSec, toSec);
        long long totalMs = latencyMs.rangeSum(fromSec, toSec);
        if (reqs == 0) return 0.0;
        return static_cast<double>(totalMs) / reqs;
    }
};
