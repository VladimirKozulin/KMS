# Инструкция и Команды по Запуску Бенчмарка MemoryGuard

Все готовые манифесты, скрипты замеров и нагрузочные сценарии подготовлены в директории `f:\projects\KMS\benchmark-bank`.

---

## Шаг 1. Запуск гетерогенного банковского кластера (46+ контейнеров)

Выполните команду в вашем терминале (PowerShell / WSL2):

```powershell
docker compose -f f:\projects\KMS\benchmark-bank\docker-compose.yml up -d --scale spring-bank-service=15 --scale node-bff-service=15 --scale go-payment-service=10 --scale postgres-db=3 --scale redis-cache=3
```

> **Состав разворачиваемого стенда:**
> * 15 контейнеров Spring Cloud / Java
> * 15 контейнеров Node.js BFF
> * 10 контейнеров Go Payment Service
> * 3 реплики PostgreSQL
> * 3 реплики Redis
> **Всего:** 46 параллельных контейнеров на готовых публичных Docker Hub образах.

---

## Шаг 2. Замер уровня Baseline (Без MemoryGuard)

Для замера исходного потребления памяти выполните:

```bash
python f:/projects/KMS/benchmark-bank/scripts/collector.py
```

А также запустите нагрузочный тест k6 для генерации постоянного транзакционного потока:

```powershell
Get-Content f:\projects\KMS\benchmark-bank\load\k6-polyglot-bank.js | docker run --rm -i --net=host grafana/k6 run -
```

---

## Шаг 3. Запуск MemoryGuard (Активация Оптимизации Памяти)

Запустите исследуемый контейнер **MemoryGuard** с необходимыми Linux-привилегиями:

```bash
docker run -d \
  --name memoryguard \
  --privileged \
  --pid=host \
  --cap-add=SYS_PTRACE \
  --security-opt seccomp=unconfined \
  -v /sys:/sys \
  -v /var/run/docker.sock:/var/run/docker.sock \
  -p 8787:8787 \
  memoryguard:latest
```

Дашборд MemoryGuard будет доступен по адресу **`http://localhost:8787`**.

---

## Шаг 4. Сбор итоговых результатов эффективности

Спустя 5–10 минут работы под нагрузкой снова запустите скрипт метрик:

```bash
python f:/projects/KMS/benchmark-bank/scripts/collector.py
```

Скрипт автоматически рассчитает:
1. `Total Docker Containers RAM` — Текущий объем памяти контейнеров.
2. `KSM Sharing Pages (Saved)` — Количество дедуплицированных страниц.
3. `RAM Saved by MemoryGuard/KSM` — Общий объем сэкономленной оперативной памяти в МБ/ГБ.
