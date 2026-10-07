"""Builds tables (results/summary.md, results/summary.json) and charts (charts/*.png)
from results/metrics.jsonl, results/latency.jsonl and results/phases.log.

Run: docker run --rm -v "$PWD:/e" -w /e python:3.12-slim sh -c "pip -q install matplotlib && python analyze.py"
"""
import json
import os
import re
from collections import defaultdict
from statistics import mean, median

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

PAGE = 4096
GiB = 1 << 30
MiB = 1 << 20
RES = os.environ.get("RESULTS", "results")
CH = os.environ.get("CHARTS", "charts")
os.makedirs(CH, exist_ok=True)

metrics = [json.loads(l) for l in open(f"{RES}/metrics.jsonl")]
# One latency file per loadgen replica → merge into 10-second buckets
buckets = defaultdict(lambda: {"phase": None, "services": defaultdict(list)})
raw_lat = []   # (generator file, record) — the summary uses these, unbucketed
for fn in sorted(os.listdir(RES)):
    if fn.startswith("latency") and fn.endswith(".jsonl"):
        for l in open(os.path.join(RES, fn)):
            rec = json.loads(l)
            raw_lat.append((fn, rec))
            b = buckets[int(rec["ts"] // 10)]
            b["phase"] = rec["phase"]
            for svc, v in rec["services"].items():
                b["services"][svc].append(v)
latency = []
for key in sorted(buckets):
    b = buckets[key]
    merged = {}
    for svc, vs in b["services"].items():
        ok = [v for v in vs if v["p50"] is not None]
        merged[svc] = {"rps": sum(v["rps"] for v in vs),
                       "p50": mean(v["p50"] for v in ok) if ok else None,
                       "p95": mean(v["p95"] for v in ok) if ok else None,
                       "p99": max(v["p99"] for v in ok) if ok else None,   # worst replica
                       "errors": sum(v["errors"] for v in vs)}
    latency.append({"ts": key * 10, "phase": b["phase"], "services": merged})

phases, events = [], []
for line in open(f"{RES}/phases.log"):
    m = re.match(r"(\d+) \S+ \S+ (\S+)(.*)", line)
    if not m:
        continue
    if m.group(2) == "EVENT":
        events.append((int(m.group(1)), m.group(3).strip()))
    else:
        phases.append((int(m.group(1)), m.group(2)))
t0 = phases[0][0]
NCPU = int(os.environ.get("NCPU", "18"))

# Phase-group order for tables
ORDER = [p for _, p in phases if p not in ("FINISHED",)]


def group_of(name):
    m = re.match(r"ksmexp-(.+?)-\d+$", name)
    return m.group(1) if m else name


def used(r):
    return r["mem"]["MemTotal"] - r["mem"]["MemAvailable"]


# ---- CPU: system/user share of all CPUs between consecutive samples --------
for prev, cur in zip(metrics, metrics[1:]):
    if "cpu" in prev and "cpu" in cur:
        d = {k: cur["cpu"][k] - prev["cpu"][k] for k in cur["cpu"]}
        total = sum(d.values()) or 1
        cur["sys_pct"] = 100 * (d["system"]) / total
        cur["busy_pct"] = 100 * (total - d["idle"] - d["iowait"]) / total


def phase_rows(phase, skip_first=0.3):
    """Samples of a phase; the first 30% are dropped as transition (except for convergence views)."""
    rows = [r for r in metrics if r["phase"] == phase]
    return rows[int(len(rows) * skip_first):] if len(rows) > 5 else rows


summary = {}
for ph in ORDER:
    if ph.startswith("x-") or ph.endswith("-prepare"):
        continue   # transitions, not states
    rows = phase_rows(ph)
    if not rows:
        continue
    last = rows[-1]
    k = last["ksm"]
    groups = defaultdict(lambda: {"mem": 0, "merging": 0, "zero": 0, "n": 0})
    for name, c in last["containers"].items():
        g = groups[group_of(name)]
        g["mem"] += c.get("mem_current") or 0
        g["merging"] += c.get("ksm_merging_pages", 0)
        g["zero"] += c.get("ksm_zero_pages", 0)
        g["n"] += 1
    svc = {}
    for s in ("java-pretouch", "java-plain", "python", "node"):
        per_gen = defaultdict(list)
        for fn, rec in raw_lat:
            if rec["phase"] == ph and s in rec["services"]:
                per_gen[fn].append(rec["services"][s])
        # drop the first 30% of every generator's windows (phase transition)
        steady = {fn: vs[int(len(vs) * 0.3):] or vs for fn, vs in per_gen.items()}
        vals = [v for vs in steady.values() for v in vs if v["p50"] is not None]
        if vals:
            svc[s] = {"rps": round(sum(mean(v["rps"] for v in vs) for vs in steady.values()), 1),
                      "p50": round(median(v["p50"] for v in vals), 1),
                      "p99": round(median(v["p99"] for v in vals), 1),
                      "p99_worst": round(max(v["p99"] for v in vals), 1),
                      "errors": sum(v["errors"] for v in vals)}
    summary[ph] = {
        "samples": len(rows),
        "used_gib": round(mean(used(r) for r in rows) / GiB, 2),
        "avail_pct": round(mean(100 * r["mem"]["MemAvailable"] / r["mem"]["MemTotal"] for r in rows), 1),
        "swap_used_gib": round(mean((r["mem"]["SwapTotal"] - r["mem"]["SwapFree"]) for r in rows) / GiB, 2),
        "pages_shared": k.get("pages_shared"), "pages_sharing": k.get("pages_sharing"),
        "ksm_zero_pages": k.get("ksm_zero_pages"),
        "saved_sharing_gib": round(k.get("pages_sharing", 0) * PAGE / GiB, 2),
        "saved_zero_gib": round(k.get("ksm_zero_pages", 0) * PAGE / GiB, 2),
        "general_profit_gib": round(k.get("general_profit", 0) / GiB, 2),
        "max_page_sharing": k.get("max_page_sharing"),
        "pages_to_scan": sorted({r["ksm"]["pages_to_scan"] for r in rows}),
        "sleep_ms": sorted({r["ksm"]["sleep_millisecs"] for r in rows}),
        "full_scans": k.get("full_scans"),
        "sys_cpu_pct": round(mean(r.get("sys_pct", 0) for r in rows), 2),
        "busy_cpu_pct": round(mean(r.get("busy_pct", 0) for r in rows), 2),
        "groups": {g: {"replicas": v["n"], "mem_gib": round(v["mem"] / GiB, 2),
                       "ksm_merging_mib": round(v["merging"] * PAGE / MiB, 1),
                       "ksm_zero_mib": round(v["zero"] * PAGE / MiB, 1)} for g, v in sorted(groups.items())},
        "latency": svc,
    }

json.dump(summary, open(f"{RES}/summary.json", "w"), indent=2, ensure_ascii=False)

# ---- Markdown tables ---------------------------------------------------------
md = ["## Сводка по фазам (среднее по устоявшейся части фазы)\n",
      "| Фаза | RAM занято, GiB | Свободно, % | Swap, GiB | Экономия sharing, GiB | Экономия zero, GiB | general_profit, GiB | max_page_sharing | pages_to_scan | sleep_ms | CPU system, % | CPU busy, % |",
      "|---|---|---|---|---|---|---|---|---|---|---|---|"]
for ph, s in summary.items():
    md.append(f"| {ph} | {s['used_gib']} | {s['avail_pct']} | {s['swap_used_gib']} | {s['saved_sharing_gib']} | "
              f"{s['saved_zero_gib']} | {s['general_profit_gib']} | {s['max_page_sharing']} | "
              f"{s['pages_to_scan'][0]}–{s['pages_to_scan'][-1]} | {s['sleep_ms'][0]}–{s['sleep_ms'][-1]} | "
              f"{s['sys_cpu_pct']} | {s['busy_cpu_pct']} |")
md += ["\n## Память по группам сервисов (конец фазы)\n"]
for ph in summary:
    if not (ph == "P1-load" or "mps" in ph):
        continue
    md += [f"\n**{ph}**\n", "| Группа | Реплик | RAM (cgroup), GiB | В KSM merged, MiB | В KSM zero, MiB |", "|---|---|---|---|---|"]
    for g, v in summary[ph]["groups"].items():
        md.append(f"| {g} | {v['replicas']} | {v['mem_gib']} | {v['ksm_merging_mib']} | {v['ksm_zero_mib']} |")
md += ["\n## Латентность (мс): медиана по 10-секундным окнам всех генераторов; худшее окно отдельно\n",
       "| Фаза | Сервис | RPS | p50 | p99 | p99 худшего окна | Ошибки |", "|---|---|---|---|---|---|---|"]
for ph, s in summary.items():
    for svc, v in s["latency"].items():
        md.append(f"| {ph} | {svc} | {v['rps']} | {v['p50']} | {v['p99']} | {v['p99_worst']} | {v['errors']} |")
open(f"{RES}/summary.md", "w", encoding="utf-8").write("\n".join(md) + "\n")

# ---- Charts --------------------------------------------------------------------
COLORS = ["#f2f2f2", "#e3ecf7"]


def shade(ax):
    spans = phases + ([] if phases[-1][1] == "FINISHED" else [(int(metrics[-1]["ts"]), "END")])
    for i, (ts, ph) in enumerate(spans[:-1]):
        end = spans[i + 1][0]
        ax.axvspan((ts - t0) / 60, (end - t0) / 60, color=COLORS[i % 2], zorder=0)
        ax.text((ts - t0) / 60 + 0.2, 0.98, ph, transform=ax.get_xaxis_transform(),
                fontsize=7, va="top", rotation=90, color="#555")
    for ts, _ in events:
        ax.axvline((ts - t0) / 60, color="#888", ls=":", lw=0.8, zorder=1)


x = [(r["ts"] - t0) / 60 for r in metrics]

# 1. Memory
fig, ax = plt.subplots(figsize=(13, 5))
shade(ax)
ax.plot(x, [used(r) / GiB for r in metrics], label="RAM занято (MemTotal − MemAvailable)", color="#1f4e8c", lw=2)
ax.plot(x, [r["ksm"].get("pages_sharing", 0) * PAGE / GiB for r in metrics], label="Экономия: pages_sharing", color="#2e8b57")
ax.plot(x, [r["ksm"].get("ksm_zero_pages", 0) * PAGE / GiB for r in metrics], label="Экономия: ksm_zero_pages", color="#c98a00")
ax.plot(x, [(r["mem"]["SwapTotal"] - r["mem"]["SwapFree"]) / GiB for r in metrics], label="Swap", color="#b03a2e", ls="--")
ax.set_xlabel("минуты от начала"); ax.set_ylabel("GiB"); ax.set_title("Память хоста Docker и экономия KSM")
ax.legend(loc="center left", fontsize=8); ax.grid(alpha=.3)
fig.tight_layout(); fig.savefig(f"{CH}/1_memory.png", dpi=120); plt.close(fig)

# 2. Autotune
fig, ax = plt.subplots(figsize=(13, 5))
shade(ax)
free = [100 * r["mem"]["MemAvailable"] / r["mem"]["MemTotal"] for r in metrics]
ax.plot(x, free, label="Свободно памяти, %", color="#1f4e8c", lw=2)
ax.axhline(20, color="#b03a2e", ls=":", lw=1); ax.axhline(50, color="#b03a2e", ls=":", lw=1)
ax.text(0.99, 20.5, "порог 20% (агрессивный режим ниже)", fontsize=7, color="#b03a2e", ha="right", transform=ax.get_yaxis_transform())
ax.text(0.99, 50.5, "порог 50% (медленный режим выше)", fontsize=7, color="#b03a2e", ha="right", transform=ax.get_yaxis_transform())
ax.set_ylabel("свободно, %"); ax.set_xlabel("минуты от начала")
ax2 = ax.twinx()
ax2.plot(x, [r["ksm"]["pages_to_scan"] for r in metrics], label="pages_to_scan", color="#2e8b57", lw=1)
ax2.plot(x, [r["ksm"]["sleep_millisecs"] * 10 for r in metrics], label="sleep_millisecs ×10", color="#c98a00", lw=1)
ax2.set_ylabel("параметры ksmd")
h1, l1 = ax.get_legend_handles_labels(); h2, l2 = ax2.get_legend_handles_labels()
ax.legend(h1 + h2, l1 + l2, loc="upper left", bbox_to_anchor=(0.12, 0.92), fontsize=8)
ax.set_title("Автонастройка KsmTuner в зависимости от свободной памяти")
fig.tight_layout(); fig.savefig(f"{CH}/2_autotune.png", dpi=120); plt.close(fig)

# 3. Latency p99
lx = [(l["ts"] - t0) / 60 for l in latency]
fig, ax = plt.subplots(figsize=(13, 5))
shade(ax)
for s, col in zip(("java-pretouch", "java-plain", "python", "node"), ("#1f4e8c", "#2e8b57", "#c98a00", "#7b3fa0")):
    ax.plot(lx, [l["services"].get(s, {}).get("p99") for l in latency], label=s, color=col, lw=1)
ax.set_yscale("log"); ax.set_ylabel("p99, мс (log)"); ax.set_xlabel("минуты от начала")
ax.set_title("Латентность p99 по сервисам"); ax.legend(fontsize=8); ax.grid(alpha=.3)
fig.tight_layout(); fig.savefig(f"{CH}/3_latency_p99.png", dpi=120); plt.close(fig)

# 4. CPU
fig, ax = plt.subplots(figsize=(13, 4))
shade(ax)
ax.plot(x, [r.get("sys_pct", 0) for r in metrics], label="system (ядро, включая ksmd)", color="#b03a2e")
ax.plot(x, [r.get("busy_pct", 0) for r in metrics], label="всего занято", color="#1f4e8c", alpha=.6)
ax.set_ylabel("% от всех CPU"); ax.set_xlabel("минуты от начала"); ax.legend(fontsize=8); ax.grid(alpha=.3)
ax.set_title("Загрузка CPU хоста")
fig.tight_layout(); fig.savefig(f"{CH}/4_cpu.png", dpi=120); plt.close(fig)

# 5. Per-group savings at end of P2 / P6
for ph in [p for p in summary if "mps" in p]:
    gs = summary[ph]["groups"]
    names = [g for g in gs if g in ("java-pretouch", "java-plain", "python", "node", "redis")]
    fig, ax = plt.subplots(figsize=(8, 4))
    ax.bar(names, [gs[g]["ksm_zero_mib"] / 1024 for g in names], label="zero pages", color="#c98a00")
    ax.bar(names, [gs[g]["ksm_merging_mib"] / 1024 for g in names],
           bottom=[gs[g]["ksm_zero_mib"] / 1024 for g in names], label="merged (ненулевые)", color="#2e8b57")
    ax.set_ylabel("GiB страниц в KSM"); ax.set_title(f"Что именно объединено, по группам ({ph})")
    ax.legend(fontsize=8); ax.grid(alpha=.3, axis="y")
    fig.tight_layout(); fig.savefig(f"{CH}/5_groups_{ph}.png", dpi=120); plt.close(fig)

print(open(f"{RES}/summary.md", encoding="utf-8").read())
