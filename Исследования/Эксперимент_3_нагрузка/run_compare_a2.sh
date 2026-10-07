#!/usr/bin/env bash
# Прогон 8: адаптивный тюнер (A2) после исправления триггеров, та же процедура, что run_compare.sh.
# Для каждого варианта: чистый unmerge → MemoryGuard (KSM_AUTOSTART, REINJECT=3 мин) →
# свежие java-pretouch → простой 10 мин → нагрузка 12 мин → перезапуск java-plain под нагрузкой 8 мин.
# ВНИМАНИЕ: не редактировать файл во время выполнения — bash читает скрипт по смещению.
set -u
cd "$(dirname "$0")"
export MSYS_NO_PATHCONV=1
DC="docker compose -p ksmexp -f docker-compose.experiment.yml"

log() { echo "$(date +%s) $(date '+%F %T') $*" | tee -a results/phases.log; }
phase() { echo "$1" > control/phase; echo "$2" > control/concurrency; log "$1 concurrency=$2x3 duration=${3}m"; sleep $(( $3 * 60 )); }
ksm_sysfs() { docker run --rm --privileged debian:bookworm-slim sh -c "K=/sys/kernel/mm/ksm; $1"; }

variant() {  # variant <tuner> <prefix>
    echo "$2-prepare" > control/phase
    echo 0 > control/concurrency
    $DC stop java-pretouch >> results/runner.out 2>&1
    $DC --profile optimizer rm -sf memoryguard >> results/runner.out 2>&1
    ksm_sysfs 'echo 2 > $K/run; echo 0 > $K/run; echo 100 > $K/pages_to_scan; echo 20 > $K/sleep_millisecs'
    log "EVENT $2: java-pretouch stopped, full unmerge, KSM off"
    TUNER=$1 REINJECT=3 $DC --profile optimizer up -d memoryguard >> results/runner.out 2>&1
    log "EVENT $2: memoryguard tuner=$1 started (KSM_AUTOSTART)"
    $DC start java-pretouch >> results/runner.out 2>&1
    log "EVENT $2: java-pretouch x8 started"
    phase "$2-idle" 0 10
    phase "$2-load" 16 12
    $DC restart java-plain >> results/runner.out 2>&1
    log "EVENT $2: java-plain x6 restarted"
    phase "$2-redeploy" 16 8
    $DC --profile optimizer logs -t --no-color memoryguard > "results/memoryguard_$2.log" 2>&1
}

log "RUN-COMPARE-A2-START"

variant adaptive A2
$DC --profile optimizer rm -sf memoryguard >> results/runner.out 2>&1
phase done 0 1
log "FINISHED"
