#!/usr/bin/env bash
# Сценарий эксперимента (прогон 2): фазы переключаются через ./control,
# всё логируется в results/phases.log. Нагрузка: 3 генератора × concurrency.
set -u
cd "$(dirname "$0")"
export MSYS_NO_PATHCONV=1
DC="docker compose -p ksmexp -f docker-compose.experiment.yml"
API="http://127.0.0.1:8787"
TOKEN="experiment-token"

log() { echo "$(date +%s) $(date '+%F %T') $*" | tee -a results/phases.log; }

phase() {  # phase <name> <concurrency per loadgen> <minutes>
    echo "$1" > control/phase
    echo "$2" > control/concurrency
    log "$1 concurrency=$2x3 duration=${3}m"
    sleep $(( $3 * 60 ))
}

ksm_on() {  # оператор включает KSM через API дашборда (агент сам run не трогает)
    for i in $(seq 1 30); do
        code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' \
               -H "Authorization: Bearer $TOKEN" -d '{"run":true}' $API/api/ksm/toggle)
        [ "$code" = "200" ] && { log "EVENT ksm enabled via API"; return; }
        sleep 2
    done
    log "EVENT ERROR could not enable KSM via API"
}

ksm_sysfs() {  # ksm_sysfs "<shell commands>" — привилегированный помощник оператора
    docker run --rm --privileged debian:bookworm-slim sh -c "K=/sys/kernel/mm/ksm; $1"
}

# --- Без оптимизатора ---------------------------------------------------
phase P0-idle            0   2
phase P1-load            16  4
phase P1b-high-load      48  2

# --- MemoryGuard (max_page_sharing=2, рекомендованный профиль) ----------
echo 16 > control/concurrency
MPS=2 $DC --profile optimizer up -d memoryguard >> results/runner.out 2>&1
sleep 5; ksm_on
phase P2-ksm-mps2        16  20

# Освобождение памяти: свободно > 50% → медленный режим тюнера
$DC stop java-pretouch >> results/runner.out 2>&1
log "EVENT java-pretouch x8 stopped"
phase P3-relief          16  6

# Повторный деплой: новые PID → автоинжекция, свободно < 20% → агрессивный режим
$DC start java-pretouch >> results/runner.out 2>&1
log "EVENT java-pretouch x8 started"
phase P4-redeploy        16  10

phase P5-high-load-ksm   48  3

# --- Компромисс: max_page_sharing=16 ------------------------------------
$DC --profile optimizer logs -t --no-color memoryguard > results/memoryguard_mps2.log 2>&1
$DC --profile optimizer rm -sf memoryguard >> results/runner.out 2>&1
echo 16 > control/concurrency
# Смена max_page_sharing требует полного unmerge. Делаем его, только если память это выдержит.
afford=$(ksm_sysfs 'need=$(( ($(cat $K/pages_sharing) + $(cat $K/ksm_zero_pages)) * 4 )); avail=$(awk "/MemAvailable/{print \$2}" /proc/meminfo); [ $((avail - need)) -gt 2097152 ] && echo yes || echo "no need=${need}kB avail=${avail}kB"')
log "EVENT unmerge for mps=16 affordable: $afford"
if [ "$afford" = "yes" ]; then
    echo "P6-unmerge" > control/phase
    ksm_sysfs 'echo 2 > $K/run; echo 0 > $K/run'
    log "EVENT full unmerge done (run=2)"
    MPS=16 $DC --profile optimizer up -d memoryguard >> results/runner.out 2>&1
    sleep 5; ksm_on
    phase P6-ksm-mps16   16  15
    $DC --profile optimizer logs -t --no-color memoryguard > results/memoryguard_mps16.log 2>&1
fi

phase P7-done            0   1
log "FINISHED"
