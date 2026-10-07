# Архитектура MemoryGuard

## Обзор

MemoryGuard состоит из 5 подсистем, работающих в 3 потоках внутри одного нативного процесса:

```
┌─────────────────────────────────────────────────────────────────┐
│                     MemoryGuard Process                        │
│                                                                 │
│  ┌─────────────┐  ┌───────────────────┐  ┌──────────────────┐  │
│  │  Main Thread │  │  KsmTunerThread   │  │  InjectorThread  │  │
│  │             │  │                   │  │                  │  │
│  │  ApiServer  │  │  KsmTuner         │  │  DockerClient    │  │
│  │  (port 8787)│  │  KsmAnomalyDetect │  │  madvise_injector│  │
│  └──────┬──────┘  └────────┬──────────┘  └────────┬─────────┘  │
│         │                  │                      │             │
│         ▼                  ▼                      ▼             │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │              KsmController                              │   │
│  │         /sys/kernel/mm/ksm/* (sysfs)                    │   │
│  └─────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
         │                                          │
         ▼                                          ▼
  ┌──────────────┐                        ┌──────────────────┐
  │ Linux Kernel │                        │ Docker Daemon    │
  │ ksmd daemon  │                        │ /var/run/docker  │
  └──────────────┘                        │ .sock            │
                                          └──────────────────┘
```

---

## Подсистемы

### 1. KSM-ядро (`dev.memoryguard.ksm`)

Центральная подсистема, управляющая Kernel Samepage Merging через sysfs-интерфейс.

#### KsmController

**Назначение:** чтение и запись параметров KSM из `/sys/kernel/mm/ksm/`.

**Управляемые параметры ядра:**

| Параметр sysfs | Метод Java | Описание |
|---|---|---|
| `run` | `isRunning()` / `setRunning(bool)` | Состояние демона ksmd: 0=стоп, 1=работает, 2=unmerge |
| `pages_shared` | `getPagesShared()` | Количество физических страниц, на которые есть ≥2 ссылки |
| `pages_sharing` | `getPagesSharing()` | Количество виртуальных страниц, ссылающихся на shared-страницы |
| `pages_unshared` | `getPagesUnshared()` | Страницы, просканированные ksmd, но не имеющие дубликатов |
| `pages_volatile` | `getPagesVolatile()` | Страницы, изменившиеся до завершения сканирования (CoW) |
| `full_scans` | `getFullScans()` | Количество полных проходов ksmd по всей помеченной памяти |
| `sleep_millisecs` | `getSleepMillisecs()` / `setSleepMillisecs(int)` | Пауза ksmd между сканами (мс) |
| `pages_to_scan` | `getPagesToScan()` / `setPagesToScan(int)` | Страниц за один проход сканирования |
| `use_zero_pages` | `isUseZeroPages()` / `setUseZeroPages(bool)` | Мержить ли нулевые (пустые) страницы |
| `max_page_sharing` | `getMaxPageSharing()` / `setMaxPageSharing(int)` | Макс. число виртуальных страниц на одну физическую |
| `merge_across_nodes` | `isMergeAcrossNodes()` / `setMergeAcrossNodes(bool)` | Мержить ли между NUMA-узлами |
| `stable_node_chains` | `getStableNodeChains()` | Глубина дерева дедупликации |

**Security Hardening:** метод `applySecurityHardening()` при старте устанавливает:
- `max_page_sharing = 2` (mitigation CVE-2024-0564)
- `merge_across_nodes = 0` (NUMA-изоляция)
- `use_zero_pages = 1` (безопасная оптимизация)

#### KsmTuner

**Назначение:** фоновый демон-поток, адаптивно регулирующий параметры KSM.

Два режима (`MEMORYGUARD_TUNER`):

**`adaptive` (по умолчанию)** — `AdaptiveScanPolicy`, обратная связь по тому, что сканирование реально даёт:

| Состояние | Условие | Скорость ksmd |
|---|---|---|
| CONVERGE | старт, unmerge, новая помеченная память ≥ 64 MiB, свободная память упала на ≥ 5 п.п. | потолок давления, пока не пройдут 2 полных прохода (KSM сливает страницу только на 2-м проходе) |
| TRACK: рост | чистая эффективность (Δэкономии / просканировано, окно ~2 мин) ≥ 1% | ×1.5 |
| TRACK: churn | чистая < 1%, валовая ≥ 0.1% (нагрузка ломает слитые страницы, KSM сливает другие) | плавно (×0.85) к 20% потолка |
| TRACK: тишина | валовая < 0.1% | ×0.6 до пола |

Потолок / пол по свободной памяти: < 10% — 100k / 4k стр/с; 10–20% — 50k / 2k; 20–50% — 20k / 1k; > 50% — 10k / 500. Скорость переводится в `pages_to_scan`/`sleep_millisecs` (~20 мс пачки) с jitter ±30%. Включает `smart_scan` (ядро 6.7+).

**`threshold`** — исходная схема: три фиксированных режима только по свободной памяти (ниже).

**Цикл работы `threshold` (каждые 12–20 сек, рандомизировано):**

```
┌──────────────────┐
│ Читаем /proc/    │
│ meminfo          │
└────────┬─────────┘
         │
         ▼
┌──────────────────┐     < 20% свободно     ┌──────────────────┐
│ Анализ давления  │ ──────────────────────► │ Агрессивный:     │
│ памяти           │                         │ scan=350-650     │
│                  │     20-50% свободно     │ sleep=7-13 ms    │
│                  │ ──────────────────────► ├──────────────────┤
│                  │                         │ Умеренный:       │
│                  │     > 50% свободно      │ scan=140-260     │
│                  │ ──────────────────────► │ sleep=14-26 ms   │
└──────────────────┘                         ├──────────────────┤
                                             │ Экономный:       │
                                             │ scan=70-130      │
                                             │ sleep=35-65 ms   │
                                             └────────┬─────────┘
                                                      │
                                                      ▼
                                             ┌──────────────────┐
                                             │ ±30% jitter      │
                                             │ (SecureRandom)   │
                                             └────────┬─────────┘
                                                      │
                                                      ▼
                                             ┌──────────────────┐
                                             │ Anomaly Detector │
                                             │ check()          │
                                             └────────┬─────────┘
                                                      │
                                               suspicious?
                                              ┌───┴───┐
                                              │ да    │ нет
                                              ▼       ▼
                                        ┌──────┐  ┌───────┐
                                        │Emerg.│  │Провер.│
                                        │Unmrge│  │Countdn│
                                        │run=2 │  │unmrge │
                                        └──────┘  └───────┘
```

**Антифичи безопасности:**
- Все интервалы рандомизированы через `SecureRandom` (±30%)
- Периодический unmerge (`run=2`) каждые 5–15 минут (рандомизировано)
- Emergency unmerge при обнаружении аномалии

#### KsmAnomalyDetector

**Назначение:** мониторинг KSM-метрик на предмет подозрительных паттернов, характерных для side-channel атак.

**Алгоритм:** экспоненциальное скользящее среднее (EMA) с коэффициентом α = 0.2. Аномалия фиксируется при превышении 3x от средней скорости изменения.

**3 правила детекции:**

| # | Метрика | Порог | Что означает |
|---|---|---|---|
| 1 | Скорость роста `pages_volatile` | > 3× EMA | Массовые CoW-faults = timing probe |
| 2 | Скорость роста `pages_sharing` | > 3× EMA (при avg > 10) | Memory probing / fingerprinting |
| 3 | Отношение `volatile / sharing` | > 50% (при sharing > 100) | Аномальная CoW-активность |

**Warmup:** первые 5 тиков (~1 минута) строится baseline, алерты не срабатывают.

---

### 2. Контейнерная подсистема (`dev.memoryguard.containers`)

#### DockerClient

**Назначение:** взаимодействие с Docker Engine API через Unix Domain Socket `/var/run/docker.sock`.

**Протокол:** HTTP/1.0 over Unix socket (стандартный Docker Engine API).

**Используемые эндпоинты Docker API:**

| Метод | Путь | Назначение |
|---|---|---|
| `GET` | `/containers/json` | Список запущенных контейнеров |
| `GET` | `/containers/{id}/top` | PID-ы процессов внутри контейнера |

**Получение памяти контейнера:** читается напрямую из cgroup v2/v1:
1. `/sys/fs/cgroup/system.slice/docker-{id}.scope/memory.current` (cgroup v2, systemd)
2. `/sys/fs/cgroup/docker/{id}/memory.current` (cgroup v2, без systemd)
3. `/sys/fs/cgroup/memory/docker/{id}/memory.usage_in_bytes` (cgroup v1)

**Whitelist контейнеров (модуль безопасности):**
- Переменная окружения `MEMORYGUARD_CONTAINER_WHITELIST`
- Deny-by-default: если не задана — ни один контейнер не получает `MADV_MERGEABLE`
- Формат: `nginx,redis,postgres` (проверяется по имени и образу)
- Специальное значение `*` — все контейнеры (пониженная безопасность)

**Валидация Container ID:**
- Regex `^[a-f0-9]{12,64}$`
- Предотвращает path traversal через подменённый ID в cgroup-путях

---

### 3. Ptrace-инжектор (`madvise_injector.c`)

Нативный C-бинарник, выполняющий инжекцию syscall `madvise(MADV_MERGEABLE)` в целевой процесс через механизм `ptrace`.

#### Принцип работы

```
┌───────────────────────────────────────────────────────────┐
│                  madvise_injector <PID>                    │
│                                                           │
│  1. strtol(argv[1]) → валидация PID > 1                  │
│  2. /proc/<PID>/status → проверка существования           │
│  3. fopen("/proc/<PID>/maps") → чтение карты памяти       │
│                                                           │
│  Для каждого региона (строка в maps):                     │
│  ┌─────────────────────────────────────────────────────┐  │
│  │ Фильтр 1: prot[0]=='r' && prot[3]=='p' (readable   │  │
│  │           private)                                   │  │
│  │                                                     │  │
│  │ Фильтр 2: is_sensitive_region()                     │  │
│  │   - Пропуск: [stack], [vvar], [vdso], [vsyscall]    │  │
│  │   - Пропуск: libcrypto, libssl, libgnutls и ещё 13  │  │
│  │     криптобиблиотек                                  │  │
│  │                                                     │  │
│  │ Фильтр 3: !file-backed || [heap]                    │  │
│  │   (только анонимные аллокации и кучу)                │  │
│  │                                                     │  │
│  │ Фильтр 4: len >= 8192 (MIN_REGION_SIZE)             │  │
│  │   (пропуск малых регионов с возможными секретами)    │  │
│  └─────────────────┬───────────────────────────────────┘  │
│                    │ прошёл все фильтры                    │
│                    ▼                                       │
│  ┌─────────────────────────────────────────────────────┐  │
│  │ inject_madvise_ptrace(pid, start, len):              │  │
│  │   PTRACE_ATTACH → waitpid → GETREGS                  │  │
│  │   → сохранить RIP → записать 0x050F (syscall opcode) │  │
│  │   → установить RAX=28, RDI=start, RSI=len, RDX=12   │  │
│  │   → SINGLESTEP → waitpid → проверить RAX             │  │
│  │   → восстановить RIP, регистры → DETACH              │  │
│  └─────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────┘
```

#### Blocklist чувствительных библиотек

Следующие библиотеки **НИКОГДА** не помечаются `MADV_MERGEABLE`, так как содержат криптографические ключи, пароли и токены в своей памяти:

| Библиотека | Назначение |
|---|---|
| `libcrypto`, `libssl` | OpenSSL — TLS-соединения, шифрование |
| `libgnutls` | GnuTLS — альтернативная TLS-библиотека |
| `libgcrypt` | GNU Crypto — общее шифрование |
| `libsodium` | NaCl/Sodium — высокоуровневое шифрование |
| `libgpg-error` | GPG — инфраструктура ошибок GPG |
| `libsasl` | SASL — аутентификация (LDAP, SMTP) |
| `libkrb5`, `libk5crypto` | Kerberos — сетевая аутентификация |
| `libpam` | PAM — аутентификация пользователей Linux |
| `libssh` | SSH — защищённый удалённый доступ |
| `libnss`, `libnspr` | NSS/NSPR — Mozilla Network Security Services |
| `libp11-kit` | PKCS#11 — аппаратные криптомодули |
| `libkeyutils` | Kernel Key Retention — ключи ядра |
| `libtasn1` | ASN.1 — разбор сертификатов |

---

### 4. Метрики (`dev.memoryguard.metrics`)

#### MetricsCollector

**Назначение:** агрегация всех метрик системы для API и дашборда.

**Собираемые метрики:**

| Группа | Метрики | Источник |
|---|---|---|
| KSM-статус | ksmSupported, ksmRunning, autoTune | KsmController, KsmTuner |
| KSM-страницы | pagesShared, pagesSharing, pagesUnshared, pagesVolatile | `/sys/kernel/mm/ksm/` |
| KSM-параметры | fullScans, sleepMillisecs, pagesToScan, useZeroPages | `/sys/kernel/mm/ksm/` |
| Системная память | systemTotalMemory, systemAvailableMemory, systemMemoryUsagePercent | `/proc/meminfo` |
| Экономия | savedMemoryMb, monthlySavingsUsd | Расчёт: `pagesSharing × 4096` |
| Контейнеры | id, name, image, memoryUsageBytes | Docker API + cgroup |

**Формула экономии:**
```
savedBytes = pages_sharing × 4096
savedMemoryMb = savedBytes / 1024 / 1024
monthlySavingsUsd = (savedMemoryMb / 1024) × $3.00/GB/month
```

---

### 5. Веб-подсистема (`dev.memoryguard.web`)

#### ApiServer

Встроенный HTTP-сервер на основе `com.sun.net.httpserver.HttpServer`, слушающий порт **8787**.

**Маршруты:**

| Метод | Путь | Обработчик |
|---|---|---|
| `GET` | `/` | `handleStatic` → `index.html` |
| `GET` | `/style.css`, `/app.js` | `handleStatic` → ресурсы из classpath |
| `GET` | `/api/status` | `handleStatus` → JSON-метрики |
| `POST` | `/api/ksm/toggle` | `handleToggle` → вкл/выкл KSM |
| `POST` | `/api/ksm/tune` | `handleTune` → изменение параметров |

#### Веб-дашборд

Одностраничное приложение (SPA) с Chart.js для графиков реального времени.

**Элементы UI:**
- 3 KPI-карточки: сэкономленная RAM, стоимость экономии, загрузка хоста
- График: сэкономленная RAM (MB) + загрузка системной памяти (%)
- Панель управления: переключатели KSM Enable и Auto-tune, ручная настройка параметров
- Таблица: список Docker-контейнеров с потреблением RAM

**Обновление:** polling каждые 5 секунд через `fetch('/api/status')`.

---

## Потоки выполнения

| Поток | Имя | Daemon | Интервал | Назначение |
|---|---|---|---|---|
| Main | `main` | нет | — | Инициализация, HTTP-сервер, shutdown hook |
| Tuner | `KsmTunerThread` | да | 12-20 сек (рандом) | Адаптивная настройка + anomaly detection + unmerge |
| Injector | `ContainerMadviseInjectorThread` | да | 30 сек | Обнаружение контейнеров + ptrace-инжекция |

---

## Жизненный цикл

```
Старт
  │
  ├── KsmController.isKsmSupported() → проверка /sys/kernel/mm/ksm
  │   └── если нет → System.exit(1)
  │
  ├── KsmController.applySecurityHardening()
  │   ├── max_page_sharing = 2
  │   ├── merge_across_nodes = 0
  │   └── use_zero_pages = 1
  │
  ├── KsmAnomalyDetector → создание
  │
  ├── KsmTuner → создание + запуск потока
  │
  ├── DockerClient.isDockerAvailable()
  │   ├── если да → запуск ContainerMadviseInjectorThread
  │   └── если нет → предупреждение, работа без контейнеров
  │
  ├── MetricsCollector → создание
  │
  ├── ApiServer → запуск на порту 8787
  │
  ├── ShutdownHook → регистрация
  │
  └── Thread.currentThread().join() → ожидание завершения
```
