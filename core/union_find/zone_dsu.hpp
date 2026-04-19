#pragma once
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <mutex>
#include <optional>

// DSU for server zone management
// - Servers in same zone = same component
// - Zone fails → mark entire component as dead
// - Zone recovers → re-enable component
// - Query: is this server in a live zone?

class ZoneDSU {
    std::vector<int> parent;
    std::vector<int> rank_;
    std::vector<bool> alive; // per-server alive status
    std::vector<std::string> zoneLabel; // zone name per server
    std::unordered_map<std::string, std::unordered_set<int>> zoneMembers;
    std::unordered_set<std::string> deadZones;
    mutable std::mutex mtx;

    int findRoot(int x) {
        // path compression (iterative)
        int root = x;
        while (parent[root] != root) root = parent[root];
        while (parent[x] != root) {
            int next = parent[x];
            parent[x] = root;
            x = next;
        }
        return root;
    }

public:
    explicit ZoneDSU(int n) : parent(n), rank_(n, 0), alive(n, true), zoneLabel(n) {
        for (int i = 0; i < n; i++) parent[i] = i;
    }

    // Register server into a zone
    void assignZone(int serverId, const std::string& zone) {
        std::lock_guard<std::mutex> lk(mtx);
        zoneLabel[serverId] = zone;
        zoneMembers[zone].insert(serverId);
    }

    // Union two servers into same component
    void unionServers(int a, int b) {
        std::lock_guard<std::mutex> lk(mtx);
        int ra = findRoot(a), rb = findRoot(b);
        if (ra == rb) return;
        if (rank_[ra] < rank_[rb]) std::swap(ra, rb);
        parent[rb] = ra;
        if (rank_[ra] == rank_[rb]) rank_[ra]++;
    }

    bool sameZone(int a, int b) {
        std::lock_guard<std::mutex> lk(mtx);
        return findRoot(a) == findRoot(b);
    }

    // Kill an entire zone — all servers in it become unreachable
    void killZone(const std::string& zone) {
        std::lock_guard<std::mutex> lk(mtx);
        deadZones.insert(zone);
        if (zoneMembers.count(zone)) {
            for (int sid : zoneMembers[zone]) alive[sid] = false;
        }
    }

    // Revive a zone
    void reviveZone(const std::string& zone) {
        std::lock_guard<std::mutex> lk(mtx);
        deadZones.erase(zone);
        if (zoneMembers.count(zone)) {
            for (int sid : zoneMembers[zone]) alive[sid] = true;
        }
    }

    // Kill a single server (not whole zone)
    void killServer(int serverId) {
        std::lock_guard<std::mutex> lk(mtx);
        alive[serverId] = false;
    }

    void reviveServer(int serverId) {
        std::lock_guard<std::mutex> lk(mtx);
        alive[serverId] = true;
    }

    bool isAlive(int serverId) const {
        std::lock_guard<std::mutex> lk(mtx);
        return alive[serverId];
    }

    bool isZoneDead(const std::string& zone) const {
        std::lock_guard<std::mutex> lk(mtx);
        return deadZones.count(zone) > 0;
    }

    std::string getZone(int serverId) const {
        std::lock_guard<std::mutex> lk(mtx);
        return zoneLabel[serverId];
    }

    // Get all alive servers not in dead zones
    std::vector<int> aliveServers() const {
        std::lock_guard<std::mutex> lk(mtx);
        std::vector<int> result;
        for (int i = 0; i < (int)alive.size(); i++) {
            if (alive[i] && !deadZones.count(zoneLabel[i]))
                result.push_back(i);
        }
        return result;
    }

    // Get all servers in a zone
    std::vector<int> serversInZone(const std::string& zone) const {
        std::lock_guard<std::mutex> lk(mtx);
        std::vector<int> result;
        if (zoneMembers.count(zone)) {
            for (int sid : zoneMembers.at(zone)) result.push_back(sid);
        }
        return result;
    }

    int totalServers() const { return (int)parent.size(); }
};
