#include "../balancer/load_balancer.hpp"
#include <string>

// Bridge for Python ctypes
extern "C" {
    LoadBalancer* lb_create(int n) {
        return new LoadBalancer(n);
    }

    void lb_destroy(LoadBalancer* lb) {
        delete lb;
    }

    // Returns a JSON-style string or just success for simplicity
    typedef struct {
        bool accepted;
        int serverId;
        int requestId;
        const char* reason;
    } RouteResultBridge;

    RouteResultBridge lb_route(LoadBalancer* lb, int clientId, int priority, const char* path) {
        auto res = lb->route(clientId, priority, path ? path : "/default");
        return {res.accepted, res.serverId, res.requestId, res.reason.c_str()};
    }

    void lb_complete(LoadBalancer* lb, int sid, int rid, int latency, bool failed) {
        lb->complete(sid, rid, latency, failed);
    }

    int lb_get_connections(LoadBalancer* lb, int sid) {
        return lb->getConnections(sid);
    }

    int lb_get_queue_size(LoadBalancer* lb, int sid) {
        return lb->getQueueSize(sid);
    }

    float lb_get_error_rate(LoadBalancer* lb, int sid) {
        return lb->getErrorRate(sid);
    }

    int lb_get_latency(LoadBalancer* lb, int sid) {
        return lb->getLatency(sid);
    }

    const char* lb_get_circuit_state(LoadBalancer* lb, int sid) {
        return lb->getCircuitState(sid);
    }

    void lb_kill_server(LoadBalancer* lb, int sid) {
        lb->killServer(sid);
    }

    void lb_revive_server(LoadBalancer* lb, int sid) {
        lb->reviveServer(sid);
    }

    void lb_kill_zone(LoadBalancer* lb, const char* zone) {
        if (zone) lb->killZone(std::string(zone));
    }

    void lb_revive_zone(LoadBalancer* lb, const char* zone) {
        if (zone) lb->reviveZone(std::string(zone));
    }
}
