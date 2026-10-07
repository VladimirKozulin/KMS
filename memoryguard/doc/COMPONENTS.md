# Справочник компонентов MemoryGuard

В данном документе приведено подробное техническое описание всех классов Java и модуля C, входящих в состав проекта MemoryGuard.

---

## 1. Модуль KSM (`dev.memoryguard.ksm`)

### 1.1 `KsmController.java`

Основной класс для взаимодействия с подсистемой KSM ядра Linux через файловую систему sysfs (`/sys/kernel/mm/ksm/`).

#### Поля
* `private static final Path KSM_DIR`: Путь к директории sysfs KSM (`/sys/kernel/mm/ksm`).

#### Основные методы

| Метод | Возвращаемый тип | Описание |
|---|---|---|
| `isKsmSupported()` | `boolean` | Проверяет существование директории `/sys/kernel/mm/ksm`. |
| `isRunning()` | `boolean` | Читает `/sys/kernel/mm/ksm/run`. Возвращает `true`, если значение равно `"1"`. |
| `setRunning(boolean run)` | `void` | Записывает `"1"` (включить) или `"0"` (выключить) в `run`. |
| `getPagesShared()` | `int` | Возвращает число уникальных shared-страниц в RAM (`pages_shared`). |
| `getPagesSharing()` | `int` | Возвращает число виртуальных страниц, разделяющих shared-страницы (`pages_sharing`). |
| `getPagesUnshared()` | `int` | Возвращает число страниц без дубликатов (`pages_unshared`). |
| `getPagesVolatile()` | `int` | Возвращает число быстро меняющихся страниц (`pages_volatile`). |
| `getFullScans()` | `int` | Возвращает количество полных сканирований памяти (`full_scans`). |
| `getSleepMillisecs()` | `int` | Читает интервал паузы между сканированиями (`sleep_millisecs`). |
| `setSleepMillisecs(int ms)` | `void` | Устанавливает интервал паузы `sleep_millisecs`. |
| `getPagesToScan()` | `int` | Читает количество страниц за один проход (`pages_to_scan`). |
| `setPagesToScan(int pages)` | `void` | Устанавливает значение `pages_to_scan`. |
| `isUseZeroPages()` | `boolean` | Проверяет, включено ли объединение пустых (нулевых) страниц. |
| `setUseZeroPages(boolean use)` | `void` | Управляет параметром `use_zero_pages`. |
| `getMaxPageSharing()` | `int` | Читает лимит виртуальных страниц на одну физическую (`max_page_sharing`). |
| `setMaxPageSharing(int max)` | `void` | Устанавливает `max_page_sharing` (от 1 до 256). |
| `isMergeAcrossNodes()` | `boolean` | Проверяет, включено ли объединение между NUMA-узлами. |
| `setMergeAcrossNodes(boolean merge)` | `void` | Устанавливает `merge_across_nodes` (`0` или `1`). |
| `applySecurityHardening()` | `void` | Применяет защитные настройки: `max_page_sharing=2`, `merge_across_nodes=0`, `use_zero_pages=1`. |

---

### 1.2 `KsmTuner.java`

Фоновый компонент, реализующий адаптивную настройку параметров KSM и противодействие side-channel атакам. Имплементирует `Runnable`.

#### Основные механизмы
1. **Адаптивное тюнингование**: Анализирует свободу системной RAM (`/proc/meminfo`) и корректирует `pages_to_scan` и `sleep_millisecs`.
2. **Anti-timing Jitter**: Вносит случайное отклонение ±30% в тайминги и сканируемые объёмы с использованием `SecureRandom`.
3. **Periodic Unmerge**: Каждые 20–60 циклов кратковременно выполняет сброс схлопнутых страниц (`run=2`), разрушая накапливаемые тайминг-каналы атакующих.
4. **Интеграция с Anomaly Detector**: При обнаружении аномалий экстренно сбрасывает страницы и переводит KSM в замедленный режим (`sleep=200ms`, `scan=50`).

---

### 1.3 `KsmAnomalyDetector.java`

Компонент мониторинга аномального поведения KSM на базе алгоритма экспоненциального скользящего среднего (EMA, α = 0.2).

#### Внутренний класс `AnomalyReport`
Содержит результаты проверки:
* `boolean suspicious`: Флаг наличия подозрительной активности.
* `String reason`: Описание причины аномалии.
* `int currentVolatile`, `int currentSharing`: Текущие значения метрик KSM.
* `double volatileRate`, `double sharingRate`: Скорости изменения метрик.

#### Метод `check()`
Выполняет проверку по трём правилам:
1. **Volatile Spike**: Скачок `pages_volatile` более чем в 3 раза относительно EMA baseline.
2. **Sharing Spike**: Скачок `pages_sharing` более чем в 3 раза (при baseline > 10).
3. **High Volatile Ratio**: Отношение `volatile / sharing > 50%` при объёме shared-страниц > 100.

---

## 2. Контейнерный модуль (`dev.memoryguard.containers`)

### 2.1 `DockerClient.java`

Класс взаимодействия с локальным Docker Daemon через Unix Domain Socket.

#### Вложенный класс `ContainerInfo`
* `String id`: Идентификатор контейнера (64 hex-символа).
* `String name`: Имя контейнера.
* `String image`: Имя образа.
* `long memoryUsageBytes`: Потребление оперативной памяти контейнером.

#### Основные методы
* `isDockerAvailable()`: Проверяет наличие `/var/run/docker.sock`.
* `getRunningContainers()`: Отправляет `GET /containers/json` в Docker API и возвращает список запущенных контейнеров.
* `getContainerPids(String containerId)`: Запрашивает `GET /containers/{id}/top` для получения списка PID процессов контейнера.
* `checkTrust(ContainerInfo container)`: Делегирует решение `TrustPolicy` — whitelist (точно/glob), label opt-out, домен доверия, запрет хранилищ данных (принцип Deny-by-Default).
* `isValidContainerId(String containerId)`: Валидирует формат container ID с помощью регулярного выражения `^[a-f0-9]{12,64}$`.
* `injectMadvise(int pid, String containerId)` / `unmergeProcess(...)`: Запускает `madvise_injector --container <id>`; инжектор проверяет принадлежность PID контейнеру до и после остановки процесса.

---

## 3. Модуль метрик (`dev.memoryguard.metrics`)

### 3.1 `MetricsCollector.java`

Агрегатор системных метрик для передачи в Web API.

#### Вложенный класс `MetricsReport`
Содержит полный набор собираемых данных: KSM метрики, системную память, расчёт сэкономленной памяти в МБ и финансовых средств в USD, список контейнеров.

#### Метод `collect()`
Собирает данные из `KsmController`, `KsmTuner`, `/proc/meminfo` и `DockerClient`. Расчет сэкономленных средств базируется на оценке $3.00 за 1 ГБ RAM в месяц.

---

## 4. Веб-модуль (`dev.memoryguard.web`)

### 4.1 `ApiServer.java`
Обертка над встроенным `com.sun.net.httpserver.HttpServer`.

* `start()`: Запускает HTTP-сервер на указанном порту (по умолчанию `8787`) и регистрирует контексты HTTP-обработчиков.
* `stop()`: Очищает ресурсы и останавливает сервер.

### 4.2 `ApiHandlers.java`
Содержит обработчики HTTP-запросов:
* `handleStatic(HttpExchange exchange)`: Раздаёт статический контент веб-панели из ресурсов JAR (`/web/index.html`, `/web/style.css`, `/web/app.js`).
* `handleStatus(HttpExchange exchange)`: Отдаёт агрегированные метрики системы в формате JSON (GET `/api/status`).
* `handleToggle(HttpExchange exchange)`: Включает/выключает KSM по запросу (POST `/api/ksm/toggle`).
* `handleTune(HttpExchange exchange)`: Принимает и применяет настройки ручного/автоматического управления KSM (POST `/api/ksm/tune`).

---

## 5. Нативный инжектор (`madvise_injector.c`)

Бинарный исполняемый модуль на языке C, выполняющий инжекцию вызова `madvise(addr, len, MADV_MERGEABLE)` через `ptrace`.

### Ключевые функции и структуры

#### `SENSITIVE_LIBS`
Массив строк с именами 16 криптографических и защищенных библиотек (`libssl`, `libcrypto`, `libsodium` и др.), которые исключаются из разметки дедупликации.

#### `is_sensitive_region(const char *line)`
Анализирует строки из `/proc/PID/maps`. Возвращает `1` (пропустить), если регион относится к:
* Стек-памяти (`[stack]`);
* Служебным страницам ядра (`[vvar]`, `[vdso]`, `[vsyscall]`);
* Динамическим библиотекам из списка `SENSITIVE_LIBS`.

#### `inject_madvise_ptrace(int pid, unsigned long start, unsigned long len)`
Выполняет пошаговую инжекцию системного вызова в целевой процесс:
1. Выполняет `PTRACE_ATTACH` к PID.
2. Сохраняет исходные регистры процессов (`PTRACE_GETREGS`) и текущую инструкцию (`RIP`).
3. Подменяет инструкцию на `syscall` (`0x050F`).
4. Настраивает регистры под вызов `madvise(start, len, MADV_MERGEABLE)`.
5. Выполняет одиночный шаг (`PTRACE_SINGLESTEP`).
6. Восстанавливает исходную инструкцию и значения регистров.
7. Выполняет `PTRACE_DETACH`.

#### `main(int argc, char *argv[])`
* Парсит PID с помощью безопасной функции `strtol`.
* Проверяет существование процесса через `/proc/PID/status`.
* Считывает `/proc/PID/maps` буфером 4096 байт.
* Пропускает малые регистры памяти (< 8 КБ) для снижения рисков утечки ключей небольшого размера.
* Вызывает `inject_madvise_ptrace` для каждого подходящего региона.
