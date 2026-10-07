"""Прогон 3 шёл параллельно с «осиротевшим» процессом сценария прогона 2 (bash читал
переписанный run_phases.sh по старому смещению). Метки фаз в данных перепутаны.
Этот скрипт переразмечает метрики по фактической хронологии, восстановленной по
phases.log и логам MemoryGuard. Исходный журнал сохраняется как phases_raw.log.
"""
import json
import os
import shutil

D = "results/run3"
# (начало, метка) — фактическое состояние стенда; конец = начало следующего окна
WINDOWS = [
    (1791366048, "P0-idle"),
    (1791366168, "P1-load"),
    (1791366408, "P1b-high-load"),
    (1791366548, "x-ksm-start"),            # 53 с работы с полным набором — слишком мало
    (1791366601, "S3-relief-mps2"),         # java-pretouch остановлены (сирота), свободно >50%
    (1791366975, "S4-redeploy-mps2"),       # java-pretouch перезапущены → автоинжекция, сходимость
    (1791367575, "S5-high-load-mps2"),
    (1791367748, "x-transition"),           # с 13:09:08 сценарий прогона 3 останавливает pretouch; unmerge; MemoryGuard mps=16
    (1791368142, "S7-redeploy-mps16"),      # java-pretouch перезапущены при mps=16
    (1791368743, "S8-high-load-mps16"),
    (1791368937, "x-end"),
]
EVENTS = [
    (1791366548, "EVENT memoryguard mps=2 started, ksm enabled via API"),
    (1791366601, "EVENT java-pretouch x8 stopped (orphan runner)"),
    (1791366975, "EVENT java-pretouch x8 started (orphan runner)"),
    (1791367773, "EVENT java-pretouch x8 stopped"),
    (1791367802, "EVENT full unmerge, memoryguard restarted with mps=16 (orphan runner)"),
    (1791368142, "EVENT java-pretouch x8 started"),
    (1791368937, "EVENT memoryguard removed"),
]


def label(ts):
    cur = WINDOWS[0][1]
    for start, name in WINDOWS:
        if ts >= start:
            cur = name
    return cur


def relabel(path):
    rows = [json.loads(l) for l in open(path)]
    with open(path, "w") as f:
        for r in rows:
            r["phase_raw"] = r.get("phase_raw", r["phase"])
            r["phase"] = label(r["ts"])
            f.write(json.dumps(r) + "\n")


if not os.path.exists(f"{D}/phases_raw.log"):
    shutil.copy(f"{D}/phases.log", f"{D}/phases_raw.log")
for fn in os.listdir(D):
    if fn.endswith(".jsonl"):
        relabel(os.path.join(D, fn))

fmt = lambda ts: __import__("time").strftime("%F %T", __import__("time").localtime(ts))
lines = [(ts, f"{ts} {fmt(ts)} {name}") for ts, name in WINDOWS if name != "x-end"]
lines += [(ts, f"{ts} {fmt(ts)} {text}") for ts, text in EVENTS]
lines.append((WINDOWS[-1][0] + 1, f"{WINDOWS[-1][0] + 1} {fmt(WINDOWS[-1][0] + 1)} FINISHED"))
with open(f"{D}/phases.log", "w") as f:
    for _, l in sorted(lines):
        f.write(l + "\n")
print(open(f"{D}/phases.log").read())
