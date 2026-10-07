## Сводка по фазам (среднее по устоявшейся части фазы)

| Фаза | RAM занято, GiB | Свободно, % | Swap, GiB | Экономия sharing, GiB | Экономия zero, GiB | general_profit, GiB | max_page_sharing | pages_to_scan | sleep_ms | CPU system, % | CPU busy, % |
|---|---|---|---|---|---|---|---|---|---|---|---|
| P0-idle | 22.36 | 12.1 | 1.8 | 0.0 | 0.0 | 0.0 | 256 | 100–100 | 20–20 | 1.91 | 6.87 |
| P1-load | 22.33 | 12.2 | 1.8 | 0.0 | 0.0 | 0.0 | 256 | 100–100 | 20–20 | 22.25 | 53.18 |
| P1b-high-load | 22.35 | 12.1 | 1.8 | 0.0 | 0.0 | 0.0 | 256 | 100–100 | 20–20 | 20.88 | 48.98 |
| S3-relief-mps2 | 10.73 | 57.8 | 0.5 | 0.0 | 0.0 | -0.01 | 2 | 70–226 | 18–65 | 3.29 | 9.83 |
| S4-redeploy-mps2 | 23.04 | 9.4 | 0.5 | 0.03 | 2.07 | 1.81 | 2 | 351–645 | 7–13 | 22.76 | 52.58 |
| S5-high-load-mps2 | 22.45 | 11.7 | 0.5 | 0.03 | 2.02 | 1.76 | 2 | 368–644 | 7–13 | 22.58 | 51.8 |
| S7-redeploy-mps16 | 23.02 | 9.5 | 0.5 | 0.06 | 2.06 | 1.82 | 16 | 358–644 | 7–13 | 23.27 | 53.77 |
| S8-high-load-mps16 | 22.6 | 11.2 | 0.5 | 0.04 | 2.02 | 1.78 | 16 | 380–647 | 7–13 | 22.48 | 51.95 |

## Память по группам сервисов (конец фазы)


**P1-load**

| Группа | Реплик | RAM (cgroup), GiB | В KSM merged, MiB | В KSM zero, MiB |
|---|---|---|---|---|
| collector | 1 | 0.02 | 0.0 | 0.0 |
| java-plain | 6 | 5.68 | 0.0 | 0.0 |
| java-pretouch | 8 | 11.99 | 0.0 | 0.0 |
| loadgen | 3 | 0.17 | 0.0 | 0.0 |
| node | 6 | 1.24 | 0.0 | 0.0 |
| python | 6 | 1.36 | 0.0 | 0.0 |
| redis | 2 | 0.35 | 0.0 | 0.0 |

**S3-relief-mps2**

| Группа | Реплик | RAM (cgroup), GiB | В KSM merged, MiB | В KSM zero, MiB |
|---|---|---|---|---|
| collector | 1 | 0.02 | 0.0 | 0.0 |
| java-plain | 6 | 5.68 | 0.0 | 0.0 |
| java-pretouch | 5 | 7.88 | 0.0 | 0.0 |
| loadgen | 3 | 0.17 | 0.0 | 0.0 |
| memoryguard | 1 | 0.05 | 0.0 | 0.0 |
| node | 6 | 1.26 | 0.0 | 0.0 |
| python | 6 | 1.36 | 0.0 | 0.0 |
| redis | 2 | 0.35 | 0.0 | 0.0 |

**S4-redeploy-mps2**

| Группа | Реплик | RAM (cgroup), GiB | В KSM merged, MiB | В KSM zero, MiB |
|---|---|---|---|---|
| collector | 1 | 0.02 | 0.0 | 0.0 |
| java-plain | 6 | 5.67 | 8.6 | 11.2 |
| java-pretouch | 8 | 10.79 | 53.7 | 2111.2 |
| loadgen | 3 | 0.17 | 0.0 | 0.0 |
| memoryguard | 1 | 0.05 | 0.0 | 0.0 |
| node | 6 | 1.24 | 1.6 | 0.4 |
| python | 6 | 1.36 | 2.3 | 0.6 |
| redis | 2 | 0.35 | 0.0 | 0.0 |

**S5-high-load-mps2**

| Группа | Реплик | RAM (cgroup), GiB | В KSM merged, MiB | В KSM zero, MiB |
|---|---|---|---|---|
| collector | 1 | 0.02 | 0.0 | 0.0 |
| java-plain | 6 | 5.67 | 8.6 | 11.1 |
| java-pretouch | 8 | 10.85 | 43.4 | 2056.0 |
| loadgen | 3 | 0.17 | 0.0 | 0.0 |
| memoryguard | 1 | 0.05 | 0.0 | 0.0 |
| node | 6 | 1.2 | 1.6 | 0.4 |
| python | 6 | 1.36 | 1.3 | 0.6 |
| redis | 2 | 0.35 | 0.0 | 0.0 |

**S7-redeploy-mps16**

| Группа | Реплик | RAM (cgroup), GiB | В KSM merged, MiB | В KSM zero, MiB |
|---|---|---|---|---|
| collector | 1 | 0.02 | 0.0 | 0.0 |
| java-plain | 6 | 5.66 | 9.3 | 18.8 |
| java-pretouch | 8 | 10.76 | 60.1 | 2091.8 |
| loadgen | 3 | 0.17 | 0.0 | 0.0 |
| memoryguard | 1 | 0.05 | 0.0 | 0.0 |
| node | 6 | 1.24 | 2.1 | 0.4 |
| python | 6 | 1.36 | 2.6 | 0.6 |
| redis | 2 | 0.35 | 0.0 | 0.0 |

**S8-high-load-mps16**

| Группа | Реплик | RAM (cgroup), GiB | В KSM merged, MiB | В KSM zero, MiB |
|---|---|---|---|---|
| collector | 1 | 0.02 | 0.0 | 0.0 |
| java-plain | 6 | 5.64 | 8.9 | 34.9 |
| java-pretouch | 8 | 10.82 | 45.3 | 2037.1 |
| loadgen | 3 | 0.17 | 0.0 | 0.0 |
| memoryguard | 1 | 0.0 | 0.0 | 0.0 |
| node | 6 | 1.23 | 2.2 | 0.5 |
| python | 6 | 1.36 | 1.3 | 0.6 |
| redis | 2 | 0.35 | 0.0 | 0.0 |

## Латентность (мс): медиана по 10-секундным окнам всех генераторов; худшее окно отдельно

| Фаза | Сервис | RPS | p50 | p99 | p99 худшего окна | Ошибки |
|---|---|---|---|---|---|---|
| P1-load | java-pretouch | 587.3 | 16.7 | 36.4 | 14117.5 | 0 |
| P1-load | java-plain | 585.0 | 16.8 | 35.9 | 15141.5 | 0 |
| P1-load | python | 587.0 | 17.0 | 36.4 | 15141.9 | 0 |
| P1-load | node | 587.4 | 16.7 | 35.9 | 11045.0 | 0 |
| P1b-high-load | java-pretouch | 588.9 | 46.4 | 115.3 | 10054.2 | 0 |
| P1b-high-load | java-plain | 588.3 | 45.8 | 115.1 | 10070.4 | 0 |
| P1b-high-load | python | 589.8 | 46.4 | 114.8 | 10055.4 | 0 |
| P1b-high-load | node | 588.7 | 46.3 | 112.9 | 10041.5 | 0 |
| S3-relief-mps2 | java-pretouch | 1.0 | 34.9 | 331.1 | 331.1 | 1538 |
| S3-relief-mps2 | java-plain | 59.9 | 1.7 | 3.5 | 54.2 | 0 |
| S3-relief-mps2 | python | 58.6 | 2.5 | 4.8 | 65.2 | 0 |
| S3-relief-mps2 | node | 59.6 | 1.8 | 3.8 | 52.8 | 0 |
| S4-redeploy-mps2 | java-pretouch | 580.7 | 16.9 | 37.4 | 12061.3 | 0 |
| S4-redeploy-mps2 | java-plain | 583.3 | 16.9 | 37.3 | 10036.5 | 0 |
| S4-redeploy-mps2 | python | 582.6 | 17.2 | 37.8 | 10028.5 | 0 |
| S4-redeploy-mps2 | node | 581.7 | 16.9 | 37.0 | 11022.0 | 0 |
| S5-high-load-mps2 | java-pretouch | 579.5 | 51.1 | 120.7 | 10072.9 | 0 |
| S5-high-load-mps2 | java-plain | 577.8 | 50.5 | 125.3 | 10043.5 | 0 |
| S5-high-load-mps2 | python | 580.4 | 50.7 | 124.2 | 10068.1 | 0 |
| S5-high-load-mps2 | node | 577.5 | 50.5 | 124.0 | 10070.3 | 0 |
| S7-redeploy-mps16 | java-pretouch | 582.3 | 17.0 | 36.8 | 13084.8 | 0 |
| S7-redeploy-mps16 | java-plain | 582.8 | 17.0 | 36.9 | 12047.8 | 0 |
| S7-redeploy-mps16 | python | 582.0 | 17.3 | 37.3 | 15142.6 | 0 |
| S7-redeploy-mps16 | node | 580.9 | 17.0 | 37.1 | 14093.3 | 0 |
| S8-high-load-mps16 | java-pretouch | 586.0 | 50.7 | 119.8 | 10055.5 | 0 |
| S8-high-load-mps16 | java-plain | 582.7 | 51.1 | 121.0 | 10048.6 | 0 |
| S8-high-load-mps16 | python | 581.0 | 50.9 | 120.9 | 13105.4 | 0 |
| S8-high-load-mps16 | node | 583.0 | 50.9 | 121.1 | 17154.9 | 0 |
