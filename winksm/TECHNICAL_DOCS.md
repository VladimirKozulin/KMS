# WinKSM — Техническая документация

## 1. Введение

### 1.1. Что такое WinKSM

WinKSM (Windows Kernel Samepage Merging) — ядерный драйвер для Windows, реализующий механизм объединения идентичных страниц оперативной памяти между процессами. Аналог подсистемы KSM (Kernel Samepage Merging) из ядра Linux.

### 1.2. Зачем это нужно

При запуске множества одинаковых процессов (Docker-контейнеры, виртуальные машины, microservices) значительная часть потребляемой RAM содержит идентичные данные:
- Библиотеки ядра и системные модули
- Код приложений (DLL, EXE sections)
- Одинаковые данные конфигурации
- Нулевые страницы (freed memory)

WinKSM автоматически находит такие страницы и объединяет их в одну физическую копию, обеспечивая экономию 30-60% потребляемой памяти.

### 1.3. Отличия от Linux KSM

| Характеристика | Linux KSM | WinKSM |
|---|---|---|
| Уровень реализации | Внутри ядра (mm/ksm.c) | Ядерный драйвер + user-mode сервис |
| Модификация PTE | Прямая (ptep_clear_flush) | Невозможна (PatchGuard) |
| Механизм шаринга | Прямая замена PTE | Section objects (ZwMapViewOfSection) |
| COW | Через page fault handler | Через Windows Memory Manager |
| Reverse mapping | Встроенный (anon_vma) | Собственная реализация |
| Opt-in | madvise(MADV_MERGEABLE) | Глобальное сканирование |
| Подписи | Не требуются | Тестовый режим или EV-сертификат |

---

## 2. Архитектура

### 2.1. Общая схема

```
┌─────────────────────────────────────────────────────────────┐
│                       User Space                            │
│                                                             │
│  ┌─────────────────────────────────────────────────────┐   │
│  │              WinKSM Service (.exe)                   │   │
│  │                                                      │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌───────────┐ │   │
│  │  │ HTTP Server  │  │   IOCTL      │  │  Config   │ │   │
│  │  │ (port 8787)  │──│  Client      │──│  Manager  │ │   │
│  │  └──────┬───────┘  └──────┬───────┘  └───────────┘ │   │
│  └─────────┼─────────────────┼─────────────────────────┘   │
│            │                 │ IOCTL                        │
├────────────┼─────────────────┼──────────────────────────────┤
│            │    Kernel Space │                              │
│  ┌─────────▼─────────────────▼──────────────────────────┐   │
│  │              WinKSM Driver (.sys)                     │   │
│  │                                                       │   │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐ │   │
│  │  │   Scanner   │  │    Trees    │  │   Sharer    │ │   │
│  │  │             │  │             │  │             │ │   │
│  │  │ • Process   │  │ • Stable    │  │ • Section   │ │   │
│  │  │   enum      │  │   (rbtree)  │  │   Create    │ │   │
│  │  │ • Memory    │  │ • Unstable  │  │ • Map into  │ │   │
│  │  │   read      │  │   (rbtree)  │  │   process   │ │   │
│  │  │ • xxhash32  │  │             │  │ • COW       │ │   │
│  │  └─────────────┘  └─────────────┘  └─────────────┘ │   │
│  │                                                       │   │
│  │  ┌─────────────┐  ┌─────────────┐                    │   │
│  │  │    Rmap     │  │    Stats    │                    │   │
│  │  │             │  │             │                    │   │
│  │  │ • Reverse   │  │ • Metrics   │                    │   │
│  │  │   mapping   │  │ • Counters  │                    │   │
│  │  │ • Hash      │  │ • Savings   │                    │   │
│  │  │   table     │  │   calc      │                    │   │
│  │  └─────────────┘  └─────────────┘                    │   │
│  └───────────────────────────────────────────────────────┘   │
│                          │                                   │
├──────────────────────────┼───────────────────────────────────┤
│                          ▼                                   │
│              Windows Memory Manager                          │
│                                                              │
│  Section Objects ─→ Page Tables ─→ Physical Memory           │
│         │                              │                     │
│         └── COW via Page Fault ────────┘                     │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

### 2.2. Компоненты

#### 2.2.1. Kernel Driver (winksm.sys)

Ядерный модуль, работающий в Ring 0. Основные функции:

- **Сканирование памяти** — перебор процессов и чтение их страниц
- **Хеширование** — xxhash32 для быстрого сравнения содержимого
- **Двухдеревьевая сортировка** — поиск дубликатов в O(log N)
- **Шаринг** — создание section objects и маппинг в процессы
- **Rmap** — отслеживание обратного маппинга (физ. страница → процессы)

#### 2.2.2. User-Mode Service (WinKSM_Service.exe)

Пользовательский процесс, работающий как Windows Service. Функции:

- **Управление драйвером** — IOCTL команды (старт/стоп/настройка)
- **HTTP сервер** — REST API для дашборда (порт 8787)
- **Конфигурация** — чтение/запись параметров

#### 2.2.3. Web Dashboard

Одностраничное веб-приложение (HTML/CSS/JS):

- **Мониторинг** — графики в реальном времени (Chart.js)
- **Управление** — включение/выключение KSM, настройка параметров
- **Статистика** — pages shared, savings, scan progress

---

## 3. Алгоритм работы

### 3.1. Сканирование

Основной цикл демон-потока в драйвере:

```
while (running):
    if (state == MERGE):
        processes = EnumerateProcesses()           // ZwQuerySystemInformation
        for each process P:
            regions = EnumerateMemory(P)           // ZwQueryVirtualMemory
            for each committed region R:
                for each 4KB page in R:
                    content = ReadPage(P, addr)    // ZwReadVirtualMemory
                    hash = xxhash32(content)        // Хеширование
                    ProcessPage(P, addr, hash)     // Логика объединения
    Sleep(sleep_millisecs)
```

#### 3.1.1. Перечисление процессов

```c
ZwQuerySystemInformation(SystemProcessInformation, buffer, size, &retLen)
```

Возвращает связный список `SYSTEM_PROCESS_INFORMATION`, каждый элемент описывает процесс:
- `UniqueProcessId` — PID
- `ImageName` — имя образа
- `WorkingSetSize` — размер рабочего набора

#### 3.1.2. Перечисление регионов памяти

```c
ZwQueryVirtualMemory(processHandle, addr, MemoryBasicInformation, &mbi, sizeof(mbi), &retLen)
```

Возвращает `MEMORY_BASIC_INFORMATION` для каждого региона:
- `State == MEM_COMMIT` — страница физически в памяти
- `Protect` — права доступа (PAGE_READWRITE, PAGE_READONLY)
- `Type == MEM_PRIVATE` — приватная память (не section-backed)
- `RegionSize` — размер региона

**Фильтрация:** Сканируются только регионы, которые:
- Коммитнуты (`MEM_COMMIT`)
- Читаемы (`PAGE_READWRITE` или `PAGE_READONLY`)
- Не являются guard-страницами
- Приватные (`MEM_PRIVATE`)

#### 3.1.3. Чтение страниц

```c
ZwReadVirtualMemory(processHandle, address, buffer, 4096, &bytesRead)
```

Читает 4096 байт (одну страницу) из адресного пространства процесса.

### 3.2. Хеширование

Используется **xxhash32** — быстрый некриптографический хеш:

```c
uint32_t hash = xxhash32(page_content, 4096, seed);
```

**Почему xxhash32:**
- Скорость: ~30 GB/s на современных CPU
- Достаточная дифференцирующая способность для 4KB страниц
- Аналогично Linux KSM (который использует xxhash вместо SHA1)

**Сравнение страниц:**
1. Если хеши совпали → страницы идентичны (с очень высокой вероятностью)
2. При необходимости → полное memcmp для подтверждения

### 3.3. Двухдеревьевая архитектура

Ключевая идея, позаимствованная из Linux KSM:

#### 3.3.1. Stable Tree (стабильное дерево)

- **Содержит:** Страницы, которые **уже объединены** между процессами
- **Свойство:** Никогда не перестраивается (one-way merge)
- **Ключ:** xxhash содержимого страницы
- **Структура:** Red-black tree (O(log N) вставка/поиск)
- **Почему стабильно:** Объединённые страницы сделаны read-only → их содержимое не может измениться → порядок в дереве всегда валиден

#### 3.3.2. Unstable Tree (нестабильное дерево)

- **Содержит:** Страницы-кандидаты, которые **ещё не объединены**
- **Свойство:** Перестраивается каждое полное сканирование
- **Ключ:** xxhash содержимого страницы
- **Почему нестабильно:** Страницы могут измениться между сканированиями

#### 3.3.3. Процесс объединения

```
Новая страница P с хешем H:

1. Поиск в stable tree:
   Если нашли узел с хешем H →
     Если это та же физическая страница → пропустить (уже объединена)
     Если другая → попытаться объединить (merge)

2. Поиск в unstable tree:
   Если нашли узел с хешем H →
     Страницы идентичны → объединить обе → переместить в stable tree

3. Если нигде не нашли →
     Вставить в unstable tree как кандидата
```

### 3.4. Механизм объединения (Section Objects)

Поскольку Windows запрещает прямую модификацию PTE (PatchGuard), используется альтернативный механизм:

#### 3.4.1. Создание section object

```c
ZwCreateSection(
    &sectionHandle,         // Handle на section
    SECTION_ALL_ACCESS,     // Права доступа
    &objAttr,               // Атрибуты объекта
    &maxSize,               // Максимальный размер (4096)
    PAGE_READWRITE,         // Защита страниц
    SEC_COMMIT,             // Физически в памяти
    NULL                    // File handle (NULL = pagefile-backed)
);
```

Section object — это "шаблон" страницы в памяти, backed by pagefile. При первом обращении Windows выделяет физическую страницу и копирует содержимое.

#### 3.4.2. Заполнение содержимым

```c
// Маппим section временно для записи
ZwMapViewOfSection(section, NtCurrentProcess(), &tempVa, ...);

// Копируем содержимое оригинальной страницы
RtlCopyMemory(tempVa, originalPageContent, 4096);

// Размаппим
ZwUnmapViewOfSection(NtCurrentProcess(), tempVa);
```

#### 3.4.3. Маппинг в целевой процесс

```c
ZwMapViewOfSection(
    sectionHandle,              // Section для маппинга
    targetProcessHandle,        // Handle целевого процесса
    &requestedAddress,          // Запрашиваемый адрес (= оригинальный)
    0,                          // ZeroBits
    4096,                       // Размер
    NULL,                       // SectionOffset
    &viewSize,                  // Размер view
    ViewUnmap,                  // Тип маппинга
    0,                          // AllocationType
    PAGE_READONLY               // Защита (read-only для COW)
);
```

**Ключевой момент:** Если `requestedAddress` совпадает с оригинальным адресом страницы → success. Windows заменяет физическую страницу на section-backed, но виртуальный адрес сохраняется.

#### 3.4.4. Copy-on-Write

Когда процесс пытается **записать** в read-only страницу:
1. CPU генерирует page fault (#PF)
2. Windows Memory Manager обрабатывает fault
3. Выделяет новую физическую страницу
4. Копирует содержимое
5. Обновляет PTE процесса на read-write
6. Процесс продолжает работу с自己的 копией

Остальные процессы продолжают шарить оригинальную страницу.

---

## 4. Структуры данных

### 4.1. WINKSM_STABLE_NODE

```c
typedef struct _WINKSM_STABLE_NODE {
    WINKSM_RB_NODE  TreeNode;        // Узел red-black дерева
    HANDLE          SectionHandle;   // Handle section object
    PVOID           SectionBase;     // Базовый адрес маппинга
    uint32_t        SharingCount;    // Число процессов-шареров
    uint64_t        ContentHash;     // xxhash содержимого
    uint32_t        NumDups;         // Число дубликатов
    LIST_ENTRY      RmapItems;       // Список rmap items
} WINKSM_STABLE_NODE;
```

**Размер:** ~64 байта на 64-битной системе.
**Хранится:** В stable tree, одна запись на уникальную объединённую страницу.

### 4.2. WINKSM_RMAP_ITEM

```c
typedef struct _WINKSM_RMAP_ITEM {
    WINKSM_RB_NODE  TreeNode;        // Узел red-black дерева
    uint64_t        Checksum;        // xxhash страницы
    uint32_t        ProcessId;       // PID владельца
    uint64_t        VirtualAddress;  // Виртуальный адрес
    uint8_t         Age;             // Возраст (число сканирований)
    uint8_t         RemainingSkips;  // Счётчик пропусков (smart scan)
    BOOLEAN         InStableTree;    // В каком дереве находится?
    WINKSM_STABLE_NODE *StableNode;  // Обратная ссылка на stable node
    LIST_ENTRY      RmapListEntry;   // Связь со stable node
} WINKSM_RMAP_ITEM;
```

**Размер:** ~72 байта.
**Хранится:** В unstable tree (как кандидат) или в stable node's rmap list (как объединённая).

### 4.3. WINKSM_RMAP_ENTRY

```c
typedef struct _WINKSM_RMAP_ENTRY {
    LIST_ENTRY      ListEntry;       // Связь в списке
    uint64_t        PhysicalPageId;  // Идентификатор физ. страницы
    HANDLE          ProcessHandle;   // Handle процесса
    uint64_t        VirtualAddress;  // Виртуальный адрес
    uint32_t        ProcessId;       // PID
    BOOLEAN         IsActive;        // Активна ли запись?
} WINKSM_RMAP_ENTRY;
```

**Размер:** ~48 байт.
**Хранится:** В глобальном rmap list + hash table по PID.

### 4.4. WINKSM_GLOBALS

```c
typedef struct _WINKSM_GLOBALS {
    PDEVICE_OBJECT      DeviceObject;
    WINKSM_RUN_STATE    RunState;
    HANDLE              WorkerThreadHandle;
    KEVENT              WorkerStopEvent;
    KEVENT              WorkerWakeEvent;
    WINKSM_PARAMETERS   Params;
    WINKSM_STATS        Stats;
    WINKSM_RB_NODE      *StableTreeRoot;
    WINKSM_RB_NODE      *UnstableTreeRoot;
    LIST_ENTRY          ProcessList;
    LIST_ENTRY          RmapGlobalList;
    WINKSM_SCAN_CURSOR  Cursor;
    uint32_t            TrustedPids[];
} WINKSM_GLOBALS;
```

---

## 5. IOCTL Интерфейс

### 5.1. Протокол

Все IOCTL используют `METHOD_BUFFERED` — данные копируются из user-mode буфера в kernel-mode буфер.

### 5.2. Список IOCTL

| IOCTL | Входные данные | Выходные данные | Описание |
|---|---|---|---|
| `GET_STATUS` | — | `WINKSM_STATUS_RESPONSE` | Статус KSM |
| `SET_PARAMETERS` | `WINKSM_PARAMETERS` | — | Установка параметров |
| `START_SCAN` | — | — | Запуск сканирования |
| `STOP_SCAN` | — | — | Остановка сканирования |
| `GET_STATS` | — | `WINKSM_STATS` | Сбор статистики |
| `ADD_TRUSTED_PID` | `WINKSM_PID_REQUEST` | — | Добавить PID в белый список |
| `REMOVE_TRUSTED_PID` | `WINKSM_PID_REQUEST` | — | Удалить PID из белого списка |

### 5.3. Пример использования

```cpp
#include "winksm_protocol.h"

HANDLE hDevice = CreateFile(WINKSM_DEVICE_PATH, GENERIC_READ | GENERIC_WRITE,
                            0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

// Получить статус
WINKSM_STATUS_RESPONSE status;
DWORD bytesReturned;
DeviceIoControl(hDevice, IOCTL_WINKSM_GET_STATUS,
                NULL, 0, &status, sizeof(status), &bytesReturned, NULL);

printf("KSM Running: %s\n", status.IsRunning ? "Yes" : "No");
printf("Total Saved: %llu bytes\n", status.TotalSavedBytes);

// Установить параметры
WINKSM_PARAMETERS params = {0};
params.PagesToScan = 500;
params.SleepMilliseconds = 10;
DeviceIoControl(hDevice, IOCTL_WINKSM_SET_PARAMETERS,
                &params, sizeof(params), NULL, 0, &bytesReturned, NULL);

// Запустить сканирование
DeviceIoControl(hDevice, IOCTL_WINKSM_START_SCAN,
                NULL, 0, NULL, 0, &bytesReturned, NULL);

CloseHandle(hDevice);
```

---

## 6. HTTP API

### 6.1. Эндпоинты

| Метод | Путь | Описание |
|---|---|---|
| GET | `/api/status` | Текущий статус KSM |
| POST | `/api/ksm/toggle` | Включение/выключение KSM |
| POST | `/api/ksm/tune` | Настройка параметров |

### 6.2. Ответ `/api/status`

```json
{
    "ksmSupported": true,
    "ksmRunning": true,
    "pagesShared": 1234,
    "pagesSharing": 5678,
    "pagesUnshared": 90,
    "pagesVolatile": 12,
    "pagesScanned": 1000000,
    "fullScans": 42,
    "savedMemoryMb": 27.56,
    "monthlySavingsUsd": 0.08,
    "processCount": 156,
    "zeroPagesShared": 234
}
```

### 6.3. Запрос `/api/ksm/toggle`

```json
{
    "run": true
}
```

### 6.4. Запрос `/api/ksm/tune`

```json
{
    "pagesToScan": 500,
    "sleepMillisecs": 10,
    "useZeroPages": true,
    "smartScan": true,
    "autoTune": false
}
```

---

## 7. Параметры

### 7.1. Параметры сканирования

| Параметр | Тип | По умолчанию | Диапазон | Описание |
|---|---|---|---|---|
| `pages_to_scan` | uint32 | 100 | 10 — 100000 | Страниц за один батч |
| `sleep_millisecs` | uint32 | 20 | 0 — 10000 | Пауза между батчами (мс) |
| `use_zero_pages` | bool | false | — | Объединять нулевые страницы |
| `smart_scan` | bool | true | — | Пропускать volatile страницы |
| `max_page_sharing` | uint32 | 256 | 2 — 10000 | Макс. шареров на страницу |

### 7.2. Влияние на производительность

| Параметр | Высокое значение | Низкое значение |
|---|---|---|
| `pages_to_scan` | Быстрее находит дубликаты, больше CPU | Медленнее, меньше CPU |
| `sleep_millisecs` | Меньше CPU, медленнее результат | Больше CPU, быстрее результат |
| `max_page_sharing` | Больше экономии, медленнее rmap walks | Меньше экономии, быстрее |

---

## 8. Безопасность

### 8.1. PatchGuard (Kernel Protection)

WinKSM **не модифицирует** защищённые ядерные структуры:
- Не хукает SSDT
- Не изменяет IDT/GDT
- Не модифицирует PTE напрямую
- Использует только документированные Zw* API

### 8.2. Модель доверия

- IOCTL доступен только процессам с правами администратора
- Драйвер требует `SeDebugPrivilege` для чтения памяти процессов
- Дашборд слушает только на `127.0.0.1` (localhost)
- Опциональные trust groups для ограничения шаринга

### 8.3. Известные ограничения

| Ограничение | Описание | Обход |
|---|---|---|
| ASLR | Разные виртуальные адреса → нельзя шарить | Пропуск страниц с различающимися адресами |
| ZwMapViewOfSection | Может не дать запрошенный адрес | Пропуск страниц при несовпадении адреса |
| Нет прямого PTE | Невозможно атомарно заменить PTE | Использование section objects |
| Тестовый режим | Драйвер не запустится без подписи | EV-сертификат для продакшена |

---

## 9. Производительность

### 9.1. Overhead сканирования

| Операция | Стоимость |
|---|---|
| Перечисление процессов | O(N), N = число процессов |
| Чтение страницы | ~1μs на 4KB |
| xxhash32 | ~100ns на 4KB |
| Поиск в rbtree | O(log M), M = число уникальных страниц |
|memcmp (tie-breaking) | ~1μs на 4KB (при совпадении хешей) |

### 9.2. Потребление памяти

| Структура | Размер | Ограничение |
|---|---|---|
| Rmap item | 72 байт | Не более pages_sharing записей |
| Stable node | 64 байт | Не более pages_shared записей |
| Rmap entry | 48 байт | Не более pages_sharing записей |
| Page buffer | 4096 байт | Один на поток сканирования |

### 9.3. Оптимизации

- **Smart scan:** Пропуск страниц, которые меняются слишком часто (age > threshold)
- **Batch processing:** Сканирование N страниц за один вызов
- **Hash-based filtering:** O(1) проверка через хеш before expensive memcmp
- **Adaptive sleep:** Динамическая регулировка паузы между батчами

---

## 10. Сравнение с альтернативами

| Решение | Уровень | Шаринг | PatchGuard | Сложность |
|---|---|---|---|---|
| **WinKSM** | Driver | Section objects | Safe | Средняя |
| Hyper-V KMM | Hypervisor | EPT | N/A | Высокая |
| User-mode only | Service | Невозможно | Safe | Низкая |
| PTE-hacking | Driver | Прямой | **BSOD** | Невозможна |

---

## 11. Roadmap

### v1.0 (текущая)
- Базовое сканирование и объединение
- Stable/unstable tree архитектура
- HTTP API и дашборд
- IOCTL интерфейс

### v1.1 (планы)
- Adaptive scan-time advisor (как в Linux KSM)
- Trust groups (Job Object-based filtering)
- Audit mode (логирование без реального шаринга)
- Per-process statistics

### v2.0 (планы)
- NUMA-aware merging
- Huge page support (2MB/1GB)
- ETW integration для мониторинга
- GUI installer
