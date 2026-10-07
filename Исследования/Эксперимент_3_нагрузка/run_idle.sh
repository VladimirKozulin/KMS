#!/usr/bin/env bash
# Прогон 5 — KSM без нагрузки (как в исходном бенчмарке adservice -89%):
# та же процедура, что в run_ab.sh, но concurrency=0. Проверка гипотезы
# «экономия на простаивающих JVM с AlwaysPreTouch — это нулевые страницы».
# ВНИМАНИЕ: не редактировать файл во время выполнения.
set -u
cd "$(dirname "$0")"
export MSYS_NO_PATHCONV=1
DC="docker compose -p ksmexp -f docker-compose.experiment.yml"
API="http://127.0.0.1:8787"

log() { echo "$(date +%s) $(date '+%F %T') $*" | tee -a results/phases.log; }
phase() { echo "$1" > control/phase; echo "$2" > control/concurrency; log "$1 concurrency=$2x3 duration=${3}m"; sleep $(( $3 * 60 )); }
ksm_sysfs() { docker run --rm --privileged debian:bookworm-slim sh -c "K=/sys/kernel/mm/ksm; $1"; }

log "RUN5-START"
echo "R5-prepare" > control/phase
echo 0 > control/concurrency
$DC stop java-pretouch >> results/runner.out 2>&1
$DC --profile optimizer rm -sf memoryguard >> results/runner.out 2>&1
ksm_sysfs 'echo 2 > $K/run; echo 0 > $K/run'
log "EVENT java-pretouch stopped, full unmerge done"
$DC start java-pretouch >> results/runner.out 2>&1
log "EVENT java-pretouch x8 started"
phase R5-idle-no-ksm 0 3
MPS=2 $DC --profile optimizer up -d memoryguard >> results/runner.out 2>&1
for i in $(seq 1 30); do
    code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' \
           -H 'Authorization: Bearer experiment-token' -d '{"run":true}' $API/api/ksm/toggle)
    [ "$code" = "200" ] && { log "EVENT memoryguard mps=2 started, ksm enabled via API"; break; }
    sleep 2
done
phase R5-idle-ksm-mps2 0 15
# Включаем нагрузку на уже сошедшемся KSM: как быстро исчезает экономия на нулевых страницах
phase R5-load-after-idle 16 6
$DC --profile optimizer logs -t --no-color memoryguard > results/memoryguard_R5.log 2>&1
$DC --profile optimizer rm -sf memoryguard >> results/runner.out 2>&1
phase R5-done 0 1
log "FINISHED"
