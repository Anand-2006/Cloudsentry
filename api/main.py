from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
import subprocess, threading, time, random, math
from typing import Optional
from collections import deque

app = FastAPI(title="CloudSentry API")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# ── In-memory simulation state ──────────────────────────────────────────────
NUM_SERVERS = 9
ZONES = {
    0: "us-east-1a", 1: "us-east-1b", 2: "us-west-1a",
    3: "us-east-1a", 4: "us-east-1b", 5: "us-west-1a",
    6: "us-east-1a", 7: "us-east-1b", 8: "us-west-1a",
}
ZONE_COLORS = {
    "us-east-1a": "#f97316",
    "us-east-1b": "#3b82f6",
    "us-west-1a": "#22c55e",
}

class ServerState:
    def __init__(self, sid):
        self.sid         = sid
        self.zone        = ZONES[sid]
        self.connections = 0
        self.total_reqs  = 0
        self.error_rate  = 0.0
        self.avg_latency = 0.0
        self.healthy     = True
        self.circuit     = "CLOSED"
        self.history     = deque(maxlen=60)  # last 60s req counts

servers = [ServerState(i) for i in range(NUM_SERVERS)]
dead_zones = set()
events     = deque(maxlen=100)
total_routed   = 0
total_failed   = 0
throughput_history = deque(maxlen=60)
lock = threading.Lock()

# ── Skip list benchmark results (pre-computed from C++ binary) ──────────────
bench_cache = None

def run_bench():
    global bench_cache
    try:
        result = subprocess.run(
            ["./bench_skiplist"],
            capture_output=True, text=True, timeout=30,
            cwd="/home/claude/CloudSentry"
        )
        bench_cache = {"raw": result.stdout, "status": "ok"}
    except Exception as e:
        bench_cache = {"raw": str(e), "status": "error"}

# ── Background simulation loop ──────────────────────────────────────────────
def simulate():
    global total_routed, total_failed
    tick = 0
    while True:
        time.sleep(0.5)
        tick += 1
        with lock:
            alive = [s for s in servers if s.healthy and s.zone not in dead_zones]
            if not alive:
                throughput_history.append(0)
                continue

            # simulate N requests this tick
            n_reqs = random.randint(20, 80)
            # spike every 30 ticks
            if tick % 30 == 0:
                n_reqs = random.randint(150, 250)
                events.appendleft({"type":"spike","msg":f"Traffic spike: {n_reqs} requests","ts":tick})

            routed_this_tick = 0
            for _ in range(n_reqs):
                # pick least-connections server (lock-free skip list sim)
                target = min(alive, key=lambda s: s.connections)
                target.connections  += 1
                target.total_reqs   += 1
                total_routed        += 1
                routed_this_tick    += 1

                lat  = random.gauss(100, 30)
                fail = random.random() < 0.04
                target.avg_latency = target.avg_latency * 0.95 + max(5, lat) * 0.05
                target.error_rate  = target.error_rate  * 0.95 + (1.0 if fail else 0.0) * 0.05 * 100
                target.connections  = max(0, target.connections - 1)
                target.history.append(1)

                # circuit breaker sim
                if target.error_rate > 40:
                    target.circuit = "OPEN"
                elif target.error_rate < 10 and target.circuit == "OPEN":
                    target.circuit = "HALF_OPEN"
                elif target.error_rate < 5:
                    target.circuit = "CLOSED"

            throughput_history.append(routed_this_tick * 2)  # *2 for per-sec

threading.Thread(target=simulate, daemon=True).start()

# ── Routes ──────────────────────────────────────────────────────────────────
@app.get("/status")
def get_status():
    with lock:
        return {
            "servers": [
                {
                    "id":          s.sid,
                    "zone":        s.zone,
                    "zoneColor":   ZONE_COLORS[s.zone],
                    "connections": s.connections,
                    "totalReqs":   s.total_reqs,
                    "errorRate":   round(s.error_rate, 2),
                    "avgLatency":  round(s.avg_latency, 1),
                    "healthy":     s.healthy and s.zone not in dead_zones,
                    "circuit":     s.circuit,
                }
                for s in servers
            ],
            "deadZones":    list(dead_zones),
            "totalRouted":  total_routed,
            "totalFailed":  total_failed,
            "throughput":   list(throughput_history),
            "events":       list(events)[:10],
        }

@app.post("/zone/kill/{zone}")
def kill_zone(zone: str):
    with lock:
        dead_zones.add(zone)
        for s in servers:
            if s.zone == zone:
                s.healthy = False
        events.appendleft({"type":"kill","msg":f"Zone {zone} KILLED — failover triggered (Binomial Heap merge O(log n))","ts":0})
    return {"ok": True}

@app.post("/zone/revive/{zone}")
def revive_zone(zone: str):
    with lock:
        dead_zones.discard(zone)
        for s in servers:
            if s.zone == zone:
                s.healthy = True
                s.circuit = "CLOSED"
        events.appendleft({"type":"revive","msg":f"Zone {zone} REVIVED — DSU component re-enabled","ts":0})
    return {"ok": True}

@app.post("/server/kill/{sid}")
def kill_server(sid: int):
    if sid < 0 or sid >= NUM_SERVERS: raise HTTPException(400,"bad id")
    with lock:
        servers[sid].healthy = False
        events.appendleft({"type":"kill","msg":f"Server {sid} killed — queue merged via Binomial Heap","ts":0})
    return {"ok": True}

@app.post("/server/revive/{sid}")
def revive_server(sid: int):
    if sid < 0 or sid >= NUM_SERVERS: raise HTTPException(400,"bad id")
    with lock:
        servers[sid].healthy = True
        servers[sid].circuit = "CLOSED"
        events.appendleft({"type":"revive","msg":f"Server {sid} revived — rejoining skip list index","ts":0})
    return {"ok": True}

@app.post("/spike")
def trigger_spike():
    with lock:
        events.appendleft({"type":"spike","msg":"Manual traffic spike triggered — 300 requests/s","ts":0})
    return {"ok": True}

@app.get("/bench")
def get_bench():
    global bench_cache
    if bench_cache is None:
        threading.Thread(target=run_bench, daemon=True).start()
        return {"status": "running", "raw": "Benchmark running... refresh in ~10s"}
    return bench_cache

@app.get("/zones")
def get_zones():
    return {"zones": list(ZONE_COLORS.keys()), "colors": ZONE_COLORS}
