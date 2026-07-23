# Развёртывание MemoryGuard

## Системные требования

| Требование | Минимум | Рекомендация |
|---|---|---|
| ОС | Linux (x86_64) с поддержкой KSM | Ubuntu 22.04+, Debian 12+, RHEL 9+ |
| Ядро | 3.x+ с CONFIG_KSM=y | 5.15+ (cgroup v2) |
| Docker | 20.10+ | 24.0+ |
| RAM (для агента) | 32 MB | 64 MB |
| CPU (для агента) | Минимальный | Минимальный |

### Проверка KSM на хосте

```bash
# Проверить, включён ли KSM в ядре
ls /sys/kernel/mm/ksm/
# Ожидается: full_scans  max_page_sharing  merge_across_nodes  pages_shared ...

# Текущий статус
cat /sys/kernel/mm/ksm/run
# 0 = выключен, 1 = работает, 2 = unmerge

# Включить KSM (если выключен)
echo 1 > /sys/kernel/mm/ksm/run
```

---

## Сборка Docker-образа

### Стандартная сборка

```bash
cd memoryguard/
docker build -t memoryguard:latest .
```

**Этапы сборки (multi-stage Dockerfile):**

| Этап | Базовый образ | Что делает |
|---|---|---|
| `builder` | `ghcr.io/graalvm/native-image-community:21` | Maven-сборка + GraalVM Native Image + gcc для C-хелпера |
| `runtime` | `debian:bookworm-slim` | Копирует 2 бинарника (~35 MB), устанавливает procps |

**Результат:**
- `/app/memoryguard` — нативный Java-бинарник (GraalVM)
- `/app/madvise_injector` — нативный C-бинарник (ptrace-инжектор)

---

## Запуск контейнера

### Вариант 1: Максимальная безопасность (рекомендуется)

```bash
docker run -d \
  --name memoryguard \
  --pid=host \
  --cap-add=SYS_PTRACE \
  -v /sys:/sys \
  -p 8787:8787 \
  memoryguard:latest
```

> **Важно:** без `MEMORYGUARD_CONTAINER_WHITELIST` ни один контейнер НЕ будет инжектирован. KSM будет работать только с хостовыми процессами, которые сами вызвали `madvise(MADV_MERGEABLE)`.

### Вариант 2: С whitelist доверенных контейнеров

```bash
docker run -d \
  --name memoryguard \
  --pid=host \
  --cap-add=SYS_PTRACE \
  -v /sys:/sys \
  -v /var/run/docker.sock:/var/run/docker.sock \
  -p 8787:8787 \
  -e MEMORYGUARD_CONTAINER_WHITELIST=nginx,redis,postgres \
  memoryguard:latest
```

### Вариант 3: Все контейнеры (пониженная безопасность)

```bash
docker run -d \
  --name memoryguard \
  --pid=host \
  --cap-add=SYS_PTRACE \
  --security-opt seccomp=unconfined \
  -v /sys:/sys \
  -v /var/run/docker.sock:/var/run/docker.sock \
  -p 8787:8787 \
  -e MEMORYGUARD_CONTAINER_WHITELIST=* \
  memoryguard:latest
```

---

## Переменные окружения

| Переменная | Значение по умолчанию | Описание |
|---|---|---|
| `MEMORYGUARD_CONTAINER_WHITELIST` | *(не задано)* | Список контейнеров для инжекции через запятую. Пустое/отсутствует = deny-all. `*` = все. |

---

## Необходимые привилегии

| Флаг | Зачем | Обязателен? |
|---|---|---|
| `--pid=host` | Доступ к PID-пространству хоста для ptrace-инжекции | **Да** |
| `--cap-add=SYS_PTRACE` | Разрешение на ptrace attach к процессам | **Да** |
| `-v /sys:/sys` | Доступ к `/sys/kernel/mm/ksm/*` для управления KSM | **Да** |
| `-v /var/run/docker.sock:/var/run/docker.sock` | Обнаружение Docker-контейнеров и их PID | Нет (только для container discovery) |
| `--privileged` | Полный доступ ко всему | **Нет** (не рекомендуется) |
| `--security-opt seccomp=unconfined` | Отключение seccomp-фильтра | **Нет** (не рекомендуется) |

---

## Docker Compose

### Безопасная конфигурация

```yaml
# docker-compose.yml
version: '3.8'

services:
  memoryguard:
    build: .
    container_name: memoryguard
    pid: host
    cap_add:
      - SYS_PTRACE
    volumes:
      - /sys:/sys
      - /var/run/docker.sock:/var/run/docker.sock:ro
    ports:
      - "8787:8787"
    environment:
      - MEMORYGUARD_CONTAINER_WHITELIST=nginx,redis,postgres
    restart: unless-stopped
    read_only: true
    security_opt:
      - no-new-privileges:true
    tmpfs:
      - /tmp:noexec,nosuid,size=10m
```

---

## Логи и мониторинг

### Просмотр логов

```bash
docker logs memoryguard
docker logs -f memoryguard  # Follow mode
```

### Ожидаемый вывод при успешном старте

```
=== Starting MemoryGuard Agent ===
[OK] KSM kernel subsystem detected.
[SECURITY] max_page_sharing set to 2 (CVE-2024-0564 mitigation)
[SECURITY] merge_across_nodes disabled (NUMA isolation)
[SECURITY] use_zero_pages enabled (safe optimization)
[OK] KSM security hardening parameters applied.
[OK] KSM anomaly detector initialized.
[OK] Adaptive KsmTuner daemon thread started (with anti-timing + periodic unmerge).
[OK] Docker socket connection available.
[SECURITY] MEMORYGUARD_CONTAINER_WHITELIST=nginx,redis,postgres — only matching containers will be injected.
[OK] Container Madvise Injector daemon thread started.
HTTP Server started on port 8787
[OK] MemoryGuard Dashboard API Server is running on http://localhost:8787
=== MemoryGuard Agent fully started with KSM side-channel protection ===
```

### Сообщения безопасности

| Префикс | Значение |
|---|---|
| `[SECURITY]` | Действие модуля безопасности (hardening, whitelist, anomaly) |
| `[KSM-INJECT]` | Успешная ptrace-инжекция |
| `[KSM-INJECT-FAIL]` | Ошибка инжекции |
| `[OK]` | Успешная инициализация компонента |
| `[ERROR]` | Критическая ошибка |
| `[WARNING]` | Предупреждение (не критично) |

### Алерты безопасности (детектор аномалий)

```
[SECURITY] pages_volatile spike (rate=523, avg=42) — possible CoW timing attack in progress
[SECURITY] Emergency: KSM slowed down (sleep=200ms, scan=50)
[SECURITY] Periodic unmerge completed — timing channels disrupted
```

---

## Остановка и удаление

```bash
# Остановка
docker stop memoryguard

# Удаление контейнера
docker rm memoryguard

# Удаление образа
docker rmi memoryguard:latest
```

---

## Публикация в Docker Hub

```bash
# 1. Войти
docker login

# 2. Тегировать
docker tag memoryguard:latest yourusername/memoryguard:latest

# 3. Загрузить
docker push yourusername/memoryguard:latest
```

---

## Диагностика проблем

| Проблема | Причина | Решение |
|---|---|---|
| `KSM is not supported or not enabled` | KSM не включён в ядре | Проверить `ls /sys/kernel/mm/ksm/` на хосте. Если нет → ядро без `CONFIG_KSM=y` |
| `Docker socket not found` | Не примонтирован `/var/run/docker.sock` | Добавить `-v /var/run/docker.sock:/var/run/docker.sock` |
| `PTRACE_ATTACH failed` | Нет `SYS_PTRACE` capability | Добавить `--cap-add=SYS_PTRACE` |
| `deny-by-default: no containers will be injected` | `MEMORYGUARD_CONTAINER_WHITELIST` не задан | Задать через `-e MEMORYGUARD_CONTAINER_WHITELIST=...` |
| Инжектор падает с exit code 2 | Все ptrace-вызовы вернули ошибку | Проверить `--pid=host` и capabilities контейнера |
| Дашборд не открывается | Порт не проброшен | Проверить `-p 8787:8787` |
