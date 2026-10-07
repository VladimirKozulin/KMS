"""HTTP load generator for the KSM experiment.

Concurrency is read every second from /control/concurrency (0 = idle), so phases can
change without restarting containers. Every 10 s writes per-service latency
percentiles and throughput to /results/latency.jsonl.
Env: TARGETS = "name=http://host:port,..."
"""
import json
import os
import random
import threading
import time
import urllib.request

TARGETS = [t.split("=", 1) for t in os.environ["TARGETS"].split(",")]
CONTROL = "/control/concurrency"
PHASE = "/control/phase"
OUT = f"/results/latency-{os.uname().nodename}.jsonl"  # one file per loadgen replica
MAX_WORKERS = 256

lock = threading.Lock()
samples = {name: [] for name, _ in TARGETS}
errors = {name: 0 for name, _ in TARGETS}
active = 0


def read(path, default):
    try:
        with open(path) as f:
            return f.read().strip() or default
    except OSError:
        return default


def worker(idx):
    while True:
        if idx >= active:
            time.sleep(0.5)
            continue
        name, url = random.choice(TARGETS)
        t0 = time.perf_counter()
        try:
            with urllib.request.urlopen(url, timeout=10) as r:
                r.read()
            dt = (time.perf_counter() - t0) * 1000
            with lock:
                samples[name].append(dt)
        except Exception:
            with lock:
                errors[name] += 1


def pct(values, p):
    if not values:
        return None
    values = sorted(values)
    return round(values[min(len(values) - 1, int(len(values) * p))], 2)


def reporter():
    global active
    last = time.time()
    while True:
        time.sleep(1)
        active = min(MAX_WORKERS, int(read(CONTROL, "0")))
        if time.time() - last < 10:
            continue
        interval = time.time() - last
        last = time.time()
        with lock:
            snap = {k: v[:] for k, v in samples.items()}
            errs = dict(errors)
            for k in samples:
                samples[k].clear()
                errors[k] = 0
        rec = {"ts": round(last, 1), "phase": read(PHASE, "?"), "concurrency": active, "services": {}}
        for name, vals in snap.items():
            rec["services"][name] = {"rps": round(len(vals) / interval, 1), "p50": pct(vals, 0.5),
                                     "p95": pct(vals, 0.95), "p99": pct(vals, 0.99), "errors": errs[name]}
        with open(OUT, "a") as f:
            f.write(json.dumps(rec) + "\n")


for i in range(MAX_WORKERS):
    threading.Thread(target=worker, args=(i,), daemon=True).start()
reporter()
