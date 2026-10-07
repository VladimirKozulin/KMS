"""Сравнение тюнеров (прогоны 6–7): threshold (T) против adaptive (A).

Вход: results/compare/{metrics.jsonl, latency-*.jsonl, phases.log}
Выход: results/compare/compare.md, charts/compare/*.png
Run: docker run --rm -v "$PWD:/e" -w /e python:3.12-slim sh -c "pip -q install matplotlib && python compare.py"
"""
import json
import os
import re
from statistics import mean, median

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

RES = os.environ.get("RESULTS", "results/compare")
CH = os.environ.get("CHARTS", "charts/compare")
os.makedirs(CH, exist_ok=True)
GiB = 1 << 30
PAGE = 4096

metrics = [json.loads(l) for l in open(f"{RES}/metrics.jsonl")]
lat = []
for fn in os.listdir(RES):
    if fn.startswith("latency") and fn.endswith(".jsonl"):
        lat += [json.loads(l) for l in open(os.path.join(RES, fn))]

starts = {}
for line in open(f"{RES}/phases.log"):
    m = re.match(r"(\d+) \S+ \S+ EVENT (\w+): memoryguard tuner=(\w+) started", line)
    if m:
        starts[m.group(2)] = (int(m.group(1)), m.group(3))

saved = lambda r: (r["ksm"]["pages_sharing"] + r["ksm"]["ksm_zero_pages"]) * PAGE / GiB
used = lambda r: (r["mem"]["MemTotal"] - r["mem"]["MemAvailable"]) / GiB


def rows(prefix, phase=None):
    out = [r for r in metrics if r["phase"].startswith(prefix + "-") and (phase is None or r["phase"] == f"{prefix}-{phase}")]
    return out


def scan_rate(rs):
    """pages/s between first and last sample"""
    if len(rs) < 2:
        return float("nan")
    return (rs[-1]["ksm"]["pages_scanned"] - rs[0]["ksm"]["pages_scanned"]) / (rs[-1]["ts"] - rs[0]["ts"])


def sys_cpu(rs):
    vals = []
    for a, b in zip(rs, rs[1:]):
        d = {k: b["cpu"][k] - a["cpu"][k] for k in b["cpu"]}
        tot = sum(d.values()) or 1
        vals.append(100 * d["system"] / tot)
    return mean(vals) if vals else float("nan")


def latency(prefix, phase, tail=0.5):
    recs = [l for l in lat if l["phase"] == f"{prefix}-{phase}"]
    recs = sorted(recs, key=lambda l: l["ts"])
    recs = recs[int(len(recs) * (1 - tail)):]
    p50 = [v["p50"] for l in recs for v in l["services"].values() if v["p50"] is not None]
    p99 = [v["p99"] for l in recs for v in l["services"].values() if v["p99"] is not None]
    return (median(p50) if p50 else float("nan"), median(p99) if p99 else float("nan"))


result = {}
for prefix, (t0, tuner) in sorted(starts.items(), key=lambda x: x[1][0]):
    idle, load, redeploy = rows(prefix, "idle"), rows(prefix, "load"), rows(prefix, "redeploy")
    allr = rows(prefix)
    peak = max(saved(r) for r in idle)
    t90 = next((r["ts"] - t0 for r in idle if saved(r) >= 0.9 * peak), float("nan"))
    load_tail = load[len(load) // 2:]
    red_tail = redeploy[len(redeploy) // 2:]
    # integral of savings over the whole variant (GiB·min) and total pages scanned
    integral = sum((b["ts"] - a["ts"]) / 60 * (saved(a) + saved(b)) / 2 for a, b in zip(allr, allr[1:]))
    scanned = allr[-1]["ksm"]["pages_scanned"] - allr[0]["ksm"]["pages_scanned"]
    result[prefix] = dict(
        tuner=tuner,
        idle_peak=peak, idle_t90_min=t90 / 60, idle_scan=scan_rate(idle), idle_sys=sys_cpu(idle),
        idle_used_end=used(idle[-1]),
        load_saved=mean(saved(r) for r in load_tail), load_scan=scan_rate(load_tail), load_sys=sys_cpu(load_tail),
        load_lat=latency(prefix, "load"),
        red_saved=mean(saved(r) for r in red_tail), red_scan=scan_rate(red_tail),
        red_lat=latency(prefix, "redeploy"),
        integral=integral, scanned=scanned,
        eff=integral / (scanned / 1e9) if scanned else float("nan"),
    )

order = [p for p, _ in sorted(starts.items(), key=lambda x: x[1][0]) if p in result]
LABELS = {"T": "threshold", "A": "adaptive v1", "A2": "adaptive v2"}
base = result["T"]


def delta(a, t):
    return f"{(a - t) / t * 100:+.0f}%" if t else "—"


others = [p for p in order if p != "T"]
md = ["| Показатель | " + " | ".join(LABELS.get(p, p) for p in order) + " | "
      + " | ".join("Δ " + LABELS.get(p, p) for p in others) + " |",
      "|---|" + "---|" * (len(order) + len(others))]
rowsdef = [
    ("Простой: пик экономии, GiB", "idle_peak", "{:.2f}"),
    ("Простой: время до 90% пика, мин", "idle_t90_min", "{:.1f}"),
    ("Простой: скорость сканирования, стр/с", "idle_scan", "{:.0f}"),
    ("Простой: CPU system, % всех CPU", "idle_sys", "{:.2f}"),
    ("Нагрузка (2-я половина): экономия, GiB", "load_saved", "{:.2f}"),
    ("Нагрузка: скорость сканирования, стр/с", "load_scan", "{:.0f}"),
    ("Нагрузка: CPU system, %", "load_sys", "{:.2f}"),
    ("Передеплой java-plain: экономия, GiB", "red_saved", "{:.2f}"),
    ("Передеплой: скорость сканирования, стр/с", "red_scan", "{:.0f}"),
    ("Весь вариант: экономия, GiB·мин", "integral", "{:.0f}"),
    ("Весь вариант: просканировано страниц, млн", "scanned", "{:.0f}"),
    ("Эффективность: GiB·мин на 1 млрд просканированных страниц", "eff", "{:.0f}"),
]
for name, key, fmt in rowsdef:
    scale = 1e6 if key == "scanned" else 1
    vals = [result[p][key] / scale for p in order]
    md.append(f"| {name} | " + " | ".join(fmt.format(v) for v in vals) + " | "
              + " | ".join(delta(result[p][key], base[key]) for p in others) + " |")
for name, key in (("Нагрузка: латентность p50 / p99, мс", "load_lat"), ("Передеплой: латентность p50 / p99, мс", "red_lat")):
    md.append(f"| {name} | " + " | ".join(f"{result[p][key][0]:.1f} / {result[p][key][1]:.1f}" for p in order)
              + " | " + " | ".join("—" for _ in others) + " |")
open(f"{RES}/compare.md", "w", encoding="utf-8").write("\n".join(md) + "\n")
json.dump(result, open(f"{RES}/compare.json", "w"), indent=2)
print("\n".join(md))

# ---- Chart: savings and scan rate, aligned at MemoryGuard start --------------
fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(13, 7), sharex=True)
for prefix, col in (("T", "#b03a2e"), ("A", "#9ab7dd"), ("A2", "#1f4e8c")):
    if prefix not in starts:
        continue
    t0 = starts[prefix][0]
    rs = rows(prefix)
    x = [(r["ts"] - t0) / 60 for r in rs]
    ax1.plot(x, [saved(r) for r in rs], color=col, lw=2, label=LABELS.get(prefix, prefix))
    rx, ry = [], []
    for a, b in zip(rs, rs[1:]):
        if b["ts"] > a["ts"]:
            rx.append((b["ts"] - t0) / 60)
            ry.append((b["ksm"]["pages_scanned"] - a["ksm"]["pages_scanned"]) / (b["ts"] - a["ts"]))
    ax2.plot(rx, ry, color=col, lw=1.2, label=LABELS.get(prefix, prefix))
for ax in (ax1, ax2):
    for x0, x1, name in ((0, 10, "простой"), (10, 22, "нагрузка"), (22, 30, "передеплой java-plain")):
        ax.axvspan(x0, x1, color="#f2f2f2" if name != "нагрузка" else "#e3ecf7", zorder=0)
        ax.text(x0 + 0.2, 0.97, name, transform=ax.get_xaxis_transform(), fontsize=8, va="top", color="#555")
    ax.grid(alpha=.3)
    ax.legend(fontsize=8, loc="upper right")
ax1.set_ylabel("экономия, GiB"); ax1.set_title("Пороговый и адаптивный тюнер: экономия и стоимость сканирования")
ax2.set_ylabel("ksmd, страниц/с"); ax2.set_xlabel("минуты от запуска MemoryGuard")
fig.tight_layout(); fig.savefig(f"{CH}/tuner_compare.png", dpi=120); plt.close(fig)
