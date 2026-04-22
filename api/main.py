from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse
from fastapi.middleware.cors import CORSMiddleware
import subprocess, threading, time, random, json, os
from typing import Optional, List
from collections import deque

app = FastAPI(title="CloudSentry API")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# --- Simulation State ---
NUM_SERVERS = 9
ZONES = ["us-east-1a", "us-east-1b", "us-west-1a"]

class ServerInstance:
    def __init__(self, sid):
        self.sid = sid
        self.zone = ZONES[sid % 3]
        self.connections = 0
        self.totalReqs = 0
        self.errorRate = 0.0
        self.avgLatency = 0.0
        self.healthy = True
        self.circuit = "CLOSED"

servers = [ServerInstance(i) for i in range(NUM_SERVERS)]
dead_zones = set()
events = deque(maxlen=15)
throughput_history = deque([0]*60, maxlen=60)
total_routed = 0
lock = threading.Lock()

# --- Background Worker ---
def delayed_complete(sid, rid, latency, failed):
    time.sleep(latency / 1000.0)
    with lock:
        lb.complete(sid, rid, latency, failed)

def simulation_loop():
    global total_routed
    paths = ["/api/v1/resource", "/api/v2/data", "/static/image.png", "/default/index"]
    
    while True:
        # Simulate traffic waves (sine wave pattern)
        wave = abs(math.sin(time.time() / 5)) 
        base_rate = 10
        burst_rate = 80
        batch_size = int(base_rate + (wave * burst_rate))
        
        with lock:
            for _ in range(batch_size):
                client_id = random.randint(0, 50)
                path = random.choice(paths)
                priority = random.randint(1, 5)
                
                res = lb.route(client_id, priority, path)
                if res['accepted']:
                    total_routed += 1
                    # Gaussian Latency simulation
                    lat = int(random.gauss(150, 50)) 
                    lat = max(20, min(500, lat))
                    is_error = random.random() < 0.03
                    
                    threading.Thread(
                        target=delayed_complete, 
                        args=(res['serverId'], res['requestId'], lat, is_error), 
                        daemon=True
                    ).start()
                else:
                    # Log drop event to dashboard
                    if len(events) < 50:
                        events.appendleft({"type": "drop", "msg": f"Req Dropped: {res['reason']}", "ts": int(time.time())})

            throughput_history.append(batch_size)
        
        time.sleep(1)

# Start real C++ backed simulation
threading.Thread(target=simulation_loop, daemon=True).start()

# --- API Endpoints ---

@app.get("/")
def serve_dashboard():
    path = os.path.join(os.path.dirname(__file__), "..", "frontend", "index.html")
    return FileResponse(path)

@app.get("/status")
def get_status():
    with lock:
        return {
            "servers": [
                {
                    "id": s.sid,
                    "zone": s.zone,
                    "connections": s.connections,
                    "totalReqs": s.totalReqs,
                    "errorRate": round(s.errorRate, 1),
                    "avgLatency": round(s.avgLatency, 1),
                    "healthy": s.healthy and s.zone not in dead_zones,
                    "circuit": s.circuit
                } for s in servers
            ],
            "deadZones": list(dead_zones),
            "totalRouted": total_routed,
            "throughput": list(throughput_history),
            "events": list(events)
        }

@app.post("/zone/kill/{zone}")
def kill_zone(zone: str):
    with lock:
        dead_zones.add(zone)
        events.appendleft({"type": "kill", "msg": f"Zone {zone} Offline [Queue Merge O(N log N)]", "ts": int(time.time())})
    return {"ok": True}

@app.post("/zone/revive/{zone}")
def revive_zone(zone: str):
    with lock:
        dead_zones.discard(zone)
        events.appendleft({"type": "revive", "msg": f"Zone {zone} Restored [Index Rebuild O(N)]", "ts": int(time.time())})
    return {"ok": True}

@app.post("/server/kill/{sid}")
def kill_server(sid: int):
    with lock:
        if 0 <= sid < NUM_SERVERS:
            servers[sid].healthy = False
            events.appendleft({"type": "kill", "msg": f"Server {sid} Shutdown [P-Queue Extraction O(log N)]", "ts": int(time.time())})
    return {"ok": True}

@app.post("/server/revive/{sid}")
def revive_server(sid: int):
    with lock:
        if 0 <= sid < NUM_SERVERS:
            servers[sid].healthy = True
            events.appendleft({"type": "revive", "msg": f"Server {sid} Restarted [Skip List Insert O(log N)]", "ts": int(time.time())})
    return {"ok": True}

@app.get("/bench")
def run_benchmark():
    try:
        # Build and run the C++ benchmark
        base_dir = os.path.join(os.path.dirname(__file__), "..")
        # Ensure it's compiled
        subprocess.run(["g++", "-std=c++17", "benchmarks/skip_list_bench.cpp", "-o", "bench_skiplist"], 
                       cwd=base_dir, check=True)
        # Run it
        result = subprocess.run(["./bench_skiplist"], cwd=base_dir, capture_output=True, text=True)
        return json.loads(result.stdout)
    except Exception as e:
        return {"error": str(e)}

@app.get("/zones")
def get_zones():
    return {"zones": ZONES}
