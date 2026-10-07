"""Metrics collector for the KSM experiment. Runs with --pid=host --cgroupns=host.

Every INTERVAL seconds appends one JSON line to /results/metrics.jsonl with:
  - all numeric /sys/kernel/mm/ksm counters and knobs (incl. tuner-set pages_to_scan / sleep_millisecs)
  - /proc/meminfo (host view)
  - ksmd CPU time (ticks) — overhead of scanning
  - per container: cgroup memory.current, anon/file from memory.stat, and the sum of
    /proc/PID/ksm_stat over its processes (merging pages, zero pages, profit)
"""
import http.client
import json
import os
import socket
import time

INTERVAL = int(os.environ.get("INTERVAL", "10"))
KSM = "/sys/kernel/mm/ksm"
OUT = "/results/metrics.jsonl"


class UnixHTTP(http.client.HTTPConnection):
    def __init__(self):
        super().__init__("localhost")

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect("/var/run/docker.sock")


def docker(path):
    c = UnixHTTP()
    c.request("GET", path)
    return json.loads(c.getresponse().read())


def read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


def ksm_stats():
    out = {}
    for name in os.listdir(KSM):
        v = read(os.path.join(KSM, name))
        if v is None:
            continue
        try:
            out[name] = int(v)
        except ValueError:
            out[name] = v
    return out


def meminfo():
    out = {}
    for line in open("/proc/meminfo"):
        k, v = line.split(":", 1)
        out[k] = int(v.split()[0]) * 1024
    return out


def ksmd_ticks():
    for pid in os.listdir("/proc"):
        if pid.isdigit() and read(f"/proc/{pid}/comm") == "ksmd":
            stat = read(f"/proc/{pid}/stat")
            f = stat[stat.rfind(")") + 2:].split()
            return int(f[11]) + int(f[12])
    return None


def cpu_ticks():
    # ksmd is invisible from Docker Desktop's "host" PID namespace; its work is
    # kernel time, so it shows up in the host-wide "system" counter instead.
    f = open("/proc/stat").readline().split()
    names = ["user", "nice", "system", "idle", "iowait", "irq", "softirq", "steal"]
    return dict(zip(names, map(int, f[1:9])))


def cgroup_dir(cid):
    for p in (f"/sys/fs/cgroup/docker/{cid}", f"/sys/fs/cgroup/system.slice/docker-{cid}.scope"):
        if os.path.isdir(p):
            return p
    return None


def container_stats(c):
    cid = c["Id"]
    rec = {"image": c["Image"]}
    d = cgroup_dir(cid)
    if d:
        rec["mem_current"] = int(read(f"{d}/memory.current") or 0)
        st = {}
        for line in (read(f"{d}/memory.stat") or "").splitlines():
            k, v = line.split()
            st[k] = int(v)
        rec["anon"] = st.get("anon")
        rec["file"] = st.get("file")
        pids = [int(p) for p in (read(f"{d}/cgroup.procs") or "").split()]
    else:
        pids = []
    agg = {"ksm_merging_pages": 0, "ksm_zero_pages": 0, "ksm_process_profit": 0, "ksm_rmap_items": 0}
    for pid in pids:
        for line in (read(f"/proc/{pid}/ksm_stat") or "").splitlines():
            k, v = line.split()
            if k in agg:
                agg[k] += int(v)
    rec.update(agg)
    return rec


def main():
    while True:
        t0 = time.time()
        rec = {"ts": round(t0, 1), "phase": read("/control/phase") or "?",
               "ksm": ksm_stats(), "mem": meminfo(), "ksmd_ticks": ksmd_ticks(), "cpu": cpu_ticks(), "containers": {}}
        try:
            for c in docker("/containers/json"):
                name = c["Names"][0].lstrip("/")
                rec["containers"][name] = container_stats(c)
        except Exception as e:  # keep collecting host metrics even if docker API hiccups
            rec["error"] = str(e)
        with open(OUT, "a") as f:
            f.write(json.dumps(rec) + "\n")
        time.sleep(max(0, INTERVAL - (time.time() - t0)))


main()
