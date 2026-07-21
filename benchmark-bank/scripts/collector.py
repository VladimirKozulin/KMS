import os
import time
import json
import subprocess

def get_ksm_stats():
    """Чтение метрик дедупликации KSM напрямую из Linux sysfs"""
    base_path = "/sys/kernel/mm/ksm/"
    if not os.path.exists(base_path):
        return {"error": "KSM not supported or sysfs not mounted"}
    
    stats = {}
    for entry in ["pages_sharing", "pages_shared", "pages_unshared", "pages_volatile", "full_scans"]:
        file_p = os.path.join(base_path, entry)
        if os.path.exists(file_p):
            with open(file_p, "r") as f:
                try:
                    stats[entry] = int(f.read().strip())
                except ValueError:
                    stats[entry] = 0
    
    # Calculate saved RAM in MB (1 page = 4096 bytes)
    pages_sharing = stats.get("pages_sharing", 0)
    stats["saved_ram_mb"] = round((pages_sharing * 4096) / (1024 * 1024), 2)
    return stats

def get_docker_ram_stats():
    """Сбор суммарного потребления RAM контейнерами Docker"""
    try:
        res = subprocess.check_output(
            ["docker", "stats", "--no-stream", "--format", "{{.MemUsage}}"],
            text=True
        )
        total_mb = 0.0
        for line in res.strip().split("\n"):
            if not line:
                continue
            usage_str = line.split("/")[0].strip()
            if "GiB" in usage_str:
                val = float(usage_str.replace("GiB", "").strip()) * 1024
            elif "MiB" in usage_str:
                val = float(usage_str.replace("MiB", "").strip())
            elif "KiB" in usage_str:
                val = float(usage_str.replace("KiB", "").strip()) / 1024
            else:
                val = 0.0
            total_mb += val
        return round(total_mb, 2)
    except Exception as e:
        return f"Error: {e}"

if __name__ == "__main__":
    print("=== MONITORING MEMORYGUARD & KSM STATS ===")
    print(f"Timestamp: {time.strftime('%Y-%m-%d %H:%M:%S')}")
    ksm = get_ksm_stats()
    docker_ram = get_docker_ram_stats()
    
    print(f"Total Docker Containers RAM: {docker_ram} MB")
    print(f"KSM Shared Pages (Unique): {ksm.get('pages_shared', 0)}")
    print(f"KSM Sharing Pages (Saved): {ksm.get('pages_sharing', 0)}")
    print(f"RAM Saved by MemoryGuard/KSM: {ksm.get('saved_ram_mb', 0)} MB")
