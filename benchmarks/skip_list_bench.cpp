#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <random>
#include <map>
#include "../core/simple_skip_list.hpp"

using namespace std::chrono;

// A simple benchmark to compare our custom Skip List with std::map
struct Result {
    int threads;
    long long skipListOps;
    long long mapOps;
};

long long benchSkipList(int ops) {
    SimpleSkipList sl;
    auto start = high_resolution_clock::now();
    for (int i = 0; i < ops; i++) sl.insert(i % 1000, i);
    for (int i = 0; i < ops; i++) sl.search(i % 1000);
    for (int i = 0; i < ops / 2; i++) sl.remove(i % 1000);
    auto end = high_resolution_clock::now();
    return duration_cast<milliseconds>(end - start).count();
}

long long benchStdMap(int ops) {
    std::map<int, int> m;
    auto start = high_resolution_clock::now();
    for (int i = 0; i < ops; i++) m[i % 1000] = i;
    for (int i = 0; i < ops; i++) m.find(i % 1000);
    for (int i = 0; i < ops / 2; i++) m.erase(i % 1000);
    auto end = high_resolution_clock::now();
    return duration_cast<milliseconds>(end - start).count();
}

int main() {
    const int OPS = 100000;
    
    long long t1 = benchSkipList(OPS);
    long long t2 = benchStdMap(OPS);

    // Output as JSON for the Python API to read
    std::cout << "{\n";
    std::cout << "  \"ops\": " << OPS << ",\n";
    std::cout << "  \"skipListMs\": " << t1 << ",\n";
    std::cout << "  \"stdMapMs\": " << t2 << ",\n";
    std::cout << "  \"skipListTput\": " << (double)OPS / (t1 / 1000.0 + 0.001) << ",\n";
    std::cout << "  \"stdMapTput\": " << (double)OPS / (t2 / 1000.0 + 0.001) << "\n";
    std::cout << "}" << std::endl;

    return 0;
}
