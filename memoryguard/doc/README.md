# MemoryGuard — Документация

## Содержание

| Документ | Описание |
|---|---|
| [Архитектура](ARCHITECTURE.md) | Устройство системы, компоненты, потоки данных, диаграммы |
| [Безопасность](SECURITY.md) | Модель угроз KSM, 5 типов side-channel атак, 6 модулей защиты |
| [Развёртывание](DEPLOYMENT.md) | Docker-сборка, запуск, конфигурация, примеры docker-compose |
| [API](API.md) | HTTP REST API: эндпоинты, запросы, ответы, коды ошибок |
| [Справочник компонентов](COMPONENTS.md) | Детальное описание каждого Java-класса и C-модуля |

---

## Что такое MemoryGuard

**MemoryGuard** — агент оптимизации оперативной памяти для Linux-хостов, работающий на основе технологии **KSM** (Kernel Samepage Merging). Агент автоматически находит идентичные страницы памяти между Docker-контейнерами, «схлопывает» их в одну физическую страницу и экономит RAM без вмешательства в логику приложений.

### Ключевые возможности

- **Автоматическая дедупликация RAM** — через ptrace-инжекцию `madvise(MADV_MERGEABLE)` в процессы Docker-контейнеров
- **Адаптивная настройка** — `KsmTuner` динамически регулирует агрессивность сканирования в зависимости от загрузки памяти
- **Защита от side-channel атак** — 6 модулей безопасности, закрывающих уязвимости дедупликации (CoW timing, FLUSH+RELOAD, covert channels, CVE-2024-0564)
- **Веб-дашборд** — интерактивная панель мониторинга на порту 8787 с графиками реального времени
- **Нативная сборка** — GraalVM Native Image, ~35 MB бинарник без зависимости от JVM

### Технологический стек

| Компонент | Технология |
|---|---|
| Язык | Java 21 + C (инжектор) |
| Сборка | Maven + GraalVM Native Image |
| Контейнеризация | Docker multi-stage build |
| Базовый образ | `debian:bookworm-slim` |
| JSON | Gson 2.11.0 |
| HTTP-сервер | `com.sun.net.httpserver` (встроенный в JDK) |
| Фронтенд | HTML + CSS + JS + Chart.js |

---

## Быстрый старт

```bash
# 1. Сборка
cd memoryguard/
docker build -t memoryguard:latest .

# 2. Запуск (безопасный режим — deny-by-default)
docker run -d \
  --name memoryguard \
  --pid=host \
  --cap-add=SYS_PTRACE \
  -v /sys:/sys \
  -p 8787:8787 \
  memoryguard:latest

# 3. Запуск с whitelist контейнеров
docker run -d \
  --name memoryguard \
  --pid=host \
  --cap-add=SYS_PTRACE \
  -v /sys:/sys \
  -p 8787:8787 \
  -e MEMORYGUARD_CONTAINER_WHITELIST=nginx,redis,postgres \
  memoryguard:latest
```

Дашборд: **http://localhost:8787**

---

## Структура проекта

```
memoryguard/
├── Dockerfile                              # Multi-stage сборка (GraalVM → debian-slim)
├── pom.xml                                 # Maven конфигурация + GraalVM Native plugin
├── doc/                                    # Документация (вы здесь)
│   ├── README.md
│   ├── ARCHITECTURE.md
│   ├── SECURITY.md
│   ├── DEPLOYMENT.md
│   ├── API.md
│   └── COMPONENTS.md
└── src/
    └── main/
        ├── c/
        │   └── madvise_injector.c          # Ptrace-инжектор MADV_MERGEABLE
        ├── java/dev/memoryguard/
        │   ├── Main.java                   # Точка входа, интеграция всех модулей
        │   ├── containers/
        │   │   └── DockerClient.java       # Docker API через Unix socket
        │   ├── ksm/
        │   │   ├── KsmController.java      # Чтение/запись /sys/kernel/mm/ksm/*
        │   │   ├── KsmTuner.java           # Адаптивная настройка + anti-timing
        │   │   └── KsmAnomalyDetector.java # Детектор side-channel аномалий
        │   ├── metrics/
        │   │   └── MetricsCollector.java   # Сбор метрик для API
        │   └── web/
        │       ├── ApiServer.java          # HTTP-сервер
        │       └── ApiHandlers.java        # Обработчики API-эндпоинтов
        └── resources/web/
            ├── index.html                  # Веб-дашборд
            ├── app.js                      # Логика дашборда
            └── style.css                   # Стили дашборда
```
