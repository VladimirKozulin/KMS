#!/usr/bin/env bash
# Прогон 4 — чистый A/B max_page_sharing 2 vs 16 по одной процедуре:
#   остановить java-pretouch → полный unmerge (дёшево, их память ушла) →
#   MemoryGuard с нужным MPS → KSM on → запустить java-pretouch → 12 минут под нагрузкой 16x3.
# ВНИМАНИЕ: не редактировать файл во время выполнения — bash читает скрипт по смещению.
set -u
cd "$(dirname "$0")"
export MSYS_NO_PATHCONV=1
DC="docker compose -p ksmexp -f docker-compose.experiment.yml"
API="http://127.0.0.1:8787"

log() { echo "$(date +%s) $(date '+%F %T') $*" | tee -a results/phases.log; }
phase() { echo "$1" > control/phase; echo "$2" > control/concurrency; log "$1 concurrency=$2x3 duration=${3}m"; sleep $(( $3 * 60 )); }
ksm_sysfs() { docker run --rm --privileged debian:bookworm-slim sh -c "K=/sys/kernel/mm/ksm; $1"; }

run_variant() {  # run_variant <mps> <phase name>
    echo "$2-prepare" > control/phase
    echo 16 > control/concurrency
    $DC stop java-pretouch >> results/runner.out 2>&1
    $DC --profile optimizer rm -sf memoryguard >> results/runner.out 2>&1
    ksm_sysfs 'echo 2 > $K/run; echo 0 > $K/run'
    log "EVENT java-pretouch stopped, full unmerge done"
    MPS=$1 $DC --profile optimizer up -d memoryguard >> results/runner.out 2>&1
    sleep 5
    curl -s -o /dev/null -X POST -H 'Content-Type: application/json' -H 'Authorization: Bearer experiment-token' \
         -d '{"run":true}' $API/api/ksm/toggle && log "EVENT memoryguard mps=$1 started, ksm enabled via API"
    $DC start java-pretouch >> results/runner.out 2>&1
    log "EVENT java-pretouch x8 started"
    phase "$2" 16 12
    $DC --profile optimizer logs -t --no-color memoryguard > "results/memoryguard_$2.log" 2>&1
}

log "RUN4-START"
run_variant 2  R4A-mps2
run_variant 16 R4B-mps16
$DC --profile optimizer rm -sf memoryguard >> results/runner.out 2>&1
phase R4-done 0 1
log "FINISHED"
