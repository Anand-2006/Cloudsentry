from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse
from fastapi.middleware.cors import CORSMiddleware
import subprocess, threading, time, random, json, os
from typing import Optional, List
import math
from collections import deque
import ctypes

class RouteResult(ctypes.Structure):
    _fields_ = [
        ("accepted", ctypes.c_bool),
        ("serverId", ctypes.c_int),
        ("requestId", ctypes.c_int),
        ("reason", ctypes.c_char_p)
    ]

try:
    lib_path = os.path.join(os.path.dirname(__file__), "..", "build", "libcloudsentry_lib.so")
    lib = ctypes.CDLL(lib_path)

    lib.lb_create.restype = ctypes.c_void_p
    lib.lb_create.argtypes = [ctypes.c_int]

    lib.lb_destroy.argtypes = [ctypes.c_void_p]

    lib.lb_route.restype = RouteResult
    lib.lb_route.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_char_p]

    lib.lb_complete.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_bool]

    lib.lb_get_connections.restype = ctypes.c_int
    lib.lb_get_connections.argtypes = [ctypes.c_void_p, ctypes.c_int]

    lib.lb_get_queue_size.restype = ctypes.c_int
    lib.lb_get_queue_size.argtypes = [ctypes.c_void_p, ctypes.c_int]

    lib.lb_get_error_rate.restype = ctypes.c_float
    lib.lb_get_error_rate.argtypes = [ctypes.c_void_p, ctypes.c_int]

    lib.lb_get_latency.restype = ctypes.c_int
    lib.lb_get_latency.argtypes = [ctypes.c_void_p, ctypes.c_int]

    lib.lb_get_circuit_state.restype = ctypes.c_char_p
    lib.lb_get_circuit_state.argtypes = [ctypes.c_void_p, ctypes.c_int]

    lib.lb_kill_server.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.lb_revive_server.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.lb_kill_zone.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.lb_revive_zone.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

    class PyLoadBalancer:
        def __init__(self, n=9):
            self.obj = lib.lb_create(n)
        def __del__(self):
            lib.lb_destroy(self.obj)
        def route(self, client_id, priority, path="/default"):
            # Explicitly cast to c_char_p to avoid TypeError crashes
            c_path = ctypes.c_char_p(path.encode('utf-8'))
            res = lib.lb_route(self.obj, client_id, priority, c_path)
            return {
                "accepted": res.accepted,
                "serverId": res.serverId,
                "requestId": res.requestId,
                "reason": res.reason.decode('utf-8') if res.reason else "Unknown"
            }
        def complete(self, sid, rid, latency, failed):
            lib.lb_complete(self.obj, sid, rid, latency, failed)
        
        def get_metrics(self, sid):
            return {
                "connections": lib.lb_get_connections(self.obj, sid),
                "queue": lib.lb_get_queue_size(self.obj, sid),
                "errorRate": lib.lb_get_error_rate(self.obj, sid),
                "avgLatency": lib.lb_get_latency(self.obj, sid),
                "circuit": lib.lb_get_circuit_state(self.obj, sid).decode('utf-8')
            }
        
        def kill_server(self, sid): lib.lb_kill_server(self.obj, sid)
        def revive_server(self, sid): lib.lb_revive_server(self.obj, sid)
        def kill_zone(self, zone): lib.lb_kill_zone(self.obj, ctypes.c_char_p(zone.encode('utf-8')))
        def revive_zone(self, zone): lib.lb_revive_zone(self.obj, ctypes.c_char_p(zone.encode('utf-8')))

    lb = PyLoadBalancer(9)
except Exception as e:
    print(f"[ERROR] Failed to load C++ Library: {e}")
    class DummyLB:
        def route(self, cid, pr, path): return {"accepted": True, "serverId": cid % 9, "requestId": 0, "reason": "MOCK"}
        def complete(self, s, r, l, f): pass
        def get_metrics(self, sid):
            return {"connections": 0, "queue": 0, "errorRate": 0.0, "avgLatency": 0}
    lb = DummyLB()

app = FastAPI(title="CloudSentry API")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

NUM_SERVERS = 9
ZONES = ["us-east-1a", "us-east-1b", "us-west-1a"]

class ServerInstance:
    def __init__(self, sid):
        self.sid = sid
        self.zone = ZONES[sid % 3]
        self.healthy = True

servers = [ServerInstance(i) for i in range(NUM_SERVERS)]
dead_zones = set()
events = deque(maxlen=15)
throughput_history = deque([0]*60, maxlen=60)
total_routed = 0
lock = threading.Lock()

SIM_INTENSITY = 100 
MAUAL_PATH_OVERRIDE = None

def delayed_complete(sid, rid, latency, failed):
    time.sleep(latency / 1000.0)
    with lock:
        lb.complete(sid, rid, latency, failed)

def simulation_loop():
    global total_routed
    paths = ["/api/v1/resource", "/api/v2/data", "/static/images/logo.png", "/auth/login"]
    active_sessions = []

    while True:
        try:
            t = time.time()
            wave = abs(math.sin(t / 7.0)) + (math.cos(t / 11.0) * 0.3)
            base_load = 45 
            amplitude = 120 
            
            target_load = (base_load + (wave * amplitude)) * (SIM_INTENSITY / 100.0)
            
            if random.random() < 0.08:
                target_load += random.randint(100, 200)
                print(f"[Simulation] Rare Traffic Spike: {int(target_load)} req/s")

            while len(active_sessions) < 12:
                active_sessions.append([random.randint(100, 999), random.randint(3, 8), random.choice(paths)])
            
            batch_size = int(random.gauss(target_load, 25)) 
            batch_size = max(15, batch_size) 
            
            actually_routed = 0
            with lock:
                for _ in range(batch_size):
                    if random.random() < 0.7 and active_sessions:
                        idx = random.randint(0, len(active_sessions)-1)
                        client_id, rem, path = active_sessions[idx]
                        active_sessions[idx][1] -= 1
                        if active_sessions[idx][1] <= 0:
                            active_sessions.pop(idx)
                    else:
                        client_id = random.randint(1000, 5000)
                        path = random.choice(paths)
                    
                    priority = random.choice([1, 1, 2, 3, 5])
                    
                    res = lb.route(client_id, priority, path)
                    if res['accepted']:
                        total_routed += 1
                        actually_routed += 1
                        lat = int(random.gauss(1500, 300))
                        lat = max(200, min(3000, lat))
                        is_error = random.random() < 0.015
                        
                        threading.Thread(
                            target=delayed_complete, 
                            args=(res['serverId'], res['requestId'], lat, is_error), 
                            daemon=True
                        ).start()
                    else:
                        if len(events) < 50:
                            events.appendleft({"type": "drop", "msg": f"L7 Drop: {res['reason']}", "ts": int(time.time())})

                throughput_history.append(actually_routed)
            
            time.sleep(1)
        except Exception as sim_err:
            print(f"[CRITICAL] Simulation Rhythmic Loop Error: {sim_err}")
            time.sleep(2)

threading.Thread(target=simulation_loop, daemon=True).start()

@app.get("/")
def serve_dashboard():
    path = os.path.join(os.path.dirname(__file__), "..", "frontend", "index.html")
    return FileResponse(path)

@app.get("/status")
def get_status():
    with lock:
        server_stats = []
        for s in servers:
            m = lb.get_metrics(s.sid)
            server_stats.append({
                "id": s.sid,
                "zone": s.zone,
                "connections": m["connections"],
                "queue": m["queue"],
                "errorRate": m["errorRate"],
                "avgLatency": m["avgLatency"],
                "healthy": s.healthy,
                "circuit": m["circuit"]
            })

        return {
            "servers": server_stats,
            "totalRouted": total_routed,
            "events": list(events),
            "throughput": list(throughput_history),
            "deadZones": list(dead_zones),
            "simIntensity": SIM_INTENSITY,
            "chaosEnabled": CHAOS_ENABLED
        }

@app.post("/simulation/intensity")
def set_intensity(val: int):
    global SIM_INTENSITY
    SIM_INTENSITY = max(0, min(500, val))
    return {"status": "ok", "new_intensity": SIM_INTENSITY}

@app.get("/tools/lookup")
def trie_lookup(path: str):
    res = lb.route(999, 1, path)
    return {
        "path": path,
        "resolved_server_id": res["serverId"],
        "explanation": f"L7 Prefix Match found server {res['serverId']} as optimal target."
    }

@app.post("/zone/kill/{zone}")
def kill_zone(zone: str):
    with lock:
        lb.kill_zone(zone)
        dead_zones.add(zone)
        for s in servers:
            if s.zone == zone: s.healthy = False
        events.appendleft({"type": "kill", "msg": f"Zone {zone} Offline [Queue Merge O(N log N)]", "ts": int(time.time())})
    return {"ok": True}

@app.post("/zone/revive/{zone}")
def revive_zone(zone: str):
    with lock:
        lb.revive_zone(zone)
        dead_zones.discard(zone)
        for s in servers:
            if s.zone == zone: s.healthy = True
        events.appendleft({"type": "revive", "msg": f"Zone {zone} Restored [Index Rebuild O(N)]", "ts": int(time.time())})
    return {"ok": True}

@app.post("/server/kill/{sid}")
def kill_server(sid: int):
    with lock:
        if 0 <= sid < NUM_SERVERS:
            lb.kill_server(sid)
            servers[sid].healthy = False
            events.appendleft({"type": "kill", "msg": f"Server {sid} Shutdown [P-Queue Extraction O(log N)]", "ts": int(time.time())})
    return {"ok": True}

@app.post("/server/revive/{sid}")
def revive_server(sid: int):
    with lock:
        if 0 <= sid < NUM_SERVERS:
            lb.revive_server(sid)
            servers[sid].healthy = True
            events.appendleft({"type": "revive", "msg": f"Server {sid} Restarted [Skip List Insert O(log N)]", "ts": int(time.time())})
    return {"ok": True}

@app.get("/bench")
def run_benchmark():
    try:
        base_dir = os.path.join(os.path.dirname(__file__), "..")
        subprocess.run(["g++", "-std=c++17", "benchmarks/skip_list_bench.cpp", "-o", "bench_skiplist"], 
                       cwd=base_dir, check=True)
        result = subprocess.run(["./bench_skiplist"], cwd=base_dir, capture_output=True, text=True)
        return json.loads(result.stdout)
    except Exception as e:
        return {"error": str(e)}

@app.get("/zones")
def get_zones():
    return {"zones": ZONES}

# ── Chaos Monkey ──────────────────────────────────────────
CHAOS_ENABLED = False
chaos_events_log = deque(maxlen=20)

def chaos_monkey_loop():
    global CHAOS_ENABLED
    while True:
        try:
            if not CHAOS_ENABLED:
                time.sleep(1)
                continue

            time.sleep(random.uniform(8, 15))  # wait between chaos events
            if not CHAOS_ENABLED:
                continue

            with lock:
                alive_ids = [s.sid for s in servers if s.healthy]
                if len(alive_ids) <= 2:
                    # Don't kill if only 2 servers left
                    time.sleep(3)
                    continue

                # Kill 1-2 random servers
                kill_count = min(random.randint(1, 2), len(alive_ids) - 2)
                victims = random.sample(alive_ids, kill_count)
                for sid in victims:
                    lb.kill_server(sid)
                    servers[sid].healthy = False
                    msg = f"🐒 Chaos Monkey killed srv-{sid:02d}"
                    events.appendleft({"type": "chaos", "msg": msg, "ts": int(time.time())})
                    chaos_events_log.appendleft({"action": "kill", "server": sid, "ts": int(time.time())})

            # Auto-revive after 5-10 seconds
            revive_delay = random.uniform(5, 10)
            time.sleep(revive_delay)

            if not CHAOS_ENABLED:
                # If chaos was disabled during wait, still revive
                pass

            with lock:
                for sid in victims:
                    if not servers[sid].healthy:
                        lb.revive_server(sid)
                        servers[sid].healthy = True
                        msg = f"🔄 Chaos Monkey revived srv-{sid:02d}"
                        events.appendleft({"type": "revive", "msg": msg, "ts": int(time.time())})
                        chaos_events_log.appendleft({"action": "revive", "server": sid, "ts": int(time.time())})

        except Exception as e:
            print(f"[Chaos Monkey Error] {e}")
            time.sleep(3)

threading.Thread(target=chaos_monkey_loop, daemon=True).start()

@app.post("/chaos/toggle")
def toggle_chaos():
    global CHAOS_ENABLED
    CHAOS_ENABLED = not CHAOS_ENABLED
    state = "ENABLED" if CHAOS_ENABLED else "DISABLED"
    events.appendleft({"type": "chaos", "msg": f"🐒 Chaos Monkey {state}", "ts": int(time.time())})
    return {"enabled": CHAOS_ENABLED}

@app.get("/chaos/status")
def chaos_status():
    return {"enabled": CHAOS_ENABLED, "log": list(chaos_events_log)}

