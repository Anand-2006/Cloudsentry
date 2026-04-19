#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>
#include <random>
#include <iomanip>
#include "../core/skip_list/coarse_lock.hpp"
#include "../core/skip_list/fine_grained.hpp"
#include "../core/skip_list/lock_free.hpp"
#include "../core/binomial_heap/binomial_heap.hpp"

using namespace std::chrono;

struct BenchResult {
    std::string name;
    int threads;
    long long ops;
    double duration_ms;
    double throughput;
};

enum WorkloadType { WRITE_HEAVY, READ_HEAVY, MIXED };
struct WorkloadConfig { int insert_pct; int remove_pct; };

WorkloadConfig getWorkload(WorkloadType t) {
    if (t == WRITE_HEAVY) return {60, 30};
    if (t == READ_HEAVY)  return {10, 5};
    return {35, 15};
}

template<typename SkipList>
BenchResult runBench(const std::string& name, int numThreads,
                     int opsPerThread, WorkloadConfig cfg) {
    SkipList sl;
    std::atomic<long long> totalOps{0};
    for (int i = 0; i < 500; i++) sl.insert(i, i);

    auto worker = [&](int tid) {
        std::mt19937 rng(tid * 1234567);
        std::uniform_int_distribution<int> keyDist(0, 4999);
        std::uniform_int_distribution<int> opDist(0, 99);
        long long local = 0;
        for (int i = 0; i < opsPerThread; i++) {
            int op = opDist(rng), key = keyDist(rng);
            if      (op < cfg.insert_pct)                  sl.insert(key, key);
            else if (op < cfg.insert_pct + cfg.remove_pct) sl.remove(key);
            else { volatile auto r = sl.findMin(); (void)r; }
            local++;
        }
        totalOps.fetch_add(local, std::memory_order_relaxed);
    };

    std::vector<std::thread> threads;
    auto start = high_resolution_clock::now();
    for (int i = 0; i < numThreads; i++) threads.emplace_back(worker, i);
    for (auto& t : threads) t.join();
    auto end = high_resolution_clock::now();

    double ms  = duration_cast<microseconds>(end - start).count() / 1000.0;
    double tput = (totalOps.load() / ms) * 1000.0;
    return {name, numThreads, totalOps.load(), ms, tput};
}

void printResult(const BenchResult& r) {
    std::cout << std::left
              << std::setw(14) << r.name
              << std::setw(10) << r.threads
              << std::setw(12) << r.ops
              << std::setw(12) << std::fixed << std::setprecision(1) << r.duration_ms
              << std::setw(14) << std::fixed << std::setprecision(0) << r.throughput
              << "\n";
}

void printHeader() {
    std::cout << "\n" << std::string(62, '=') << "\n"
              << std::left
              << std::setw(14) << "Impl"
              << std::setw(10) << "Threads"
              << std::setw(12) << "Ops"
              << std::setw(12) << "Time(ms)"
              << std::setw(14) << "Ops/sec"
              << "\n" << std::string(62, '-') << "\n";
}

void runSuite(WorkloadType wtype, const std::string& label, int ops) {
    WorkloadConfig cfg = getWorkload(wtype);
    std::cout << "\n Workload: " << label << "\n";
    printHeader();
    for (int tc : {1, 2, 4, 8}) {
        auto r1 = runBench<CoarseLockSkipList> ("CoarseLock",  tc, ops, cfg);
        auto r2 = runBench<FineGrainedSkipList>("FineGrained", tc, ops, cfg);
        auto r3 = runBench<LockFreeSkipList>   ("LockFree",    tc, ops, cfg);
        printResult(r1); printResult(r2); printResult(r3);
        std::cout << std::string(62, '-') << "\n";
    }
}

// Binomial Heap merge vs element-wise rebuild benchmark
void binomialMergeBench() {
    std::cout << "\n Binomial Heap: O(log n) merge vs O(n log n) naive\n";
    printHeader();

    for (int sz : {100, 500, 1000, 5000}) {
        // O(log n) merge
        {
            BinomialHeap h1, h2;
            for (int i = 0; i < sz; i++) h1.insert(i, i);
            for (int i = sz; i < sz*2; i++) h2.insert(i, i);
            auto start = high_resolution_clock::now();
            h1.merge(h2);
            auto end = high_resolution_clock::now();
            double ms = duration_cast<microseconds>(end - start).count() / 1000.0;
            BenchResult r{"BH-merge", sz*2, sz*2, ms, sz*2/std::max(ms,0.001)*1000};
            r.name = "Merge n=" + std::to_string(sz);
            printResult(r);
        }
        // O(n log n) naive: extract-all + reinsert
        {
            BinomialHeap h1, h2;
            for (int i = 0; i < sz; i++) h1.insert(i, i);
            for (int i = sz; i < sz*2; i++) h2.insert(i, i);
            auto start = high_resolution_clock::now();
            while (!h2.empty()) {
                auto item = h2.extractMin();
                if (item) h1.insert(item->first, item->second);
            }
            auto end = high_resolution_clock::now();
            double ms = duration_cast<microseconds>(end - start).count() / 1000.0;
            BenchResult r{"Naive", sz*2, sz*2, ms, sz*2/std::max(ms,0.001)*1000};
            r.name = "Naive  n=" + std::to_string(sz);
            printResult(r);
        }
        std::cout << std::string(62, '-') << "\n";
    }
}

int main() {
    std::cout << "\n CloudSentry — Skip List + DS Benchmark Suite\n";
    const int OPS = 1000;
    runSuite(WRITE_HEAVY, "Write Heavy (60i/30r/10q)", OPS);
    runSuite(READ_HEAVY,  "Read Heavy  (10i/5r/85q)",  OPS);
    runSuite(MIXED,       "Mixed       (35i/15r/50q)", OPS);
    binomialMergeBench();
    std::cout << "\n Done.\n\n";
    return 0;
}
