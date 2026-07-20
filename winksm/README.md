# WinKSM - Windows Kernel Samepage Merging

Аналог Linux KSM (Kernel Samepage Merging) для Windows. Автоматическое обнаружение и объединение дублирующихся страниц оперативной памяти между процессами с Copy-on-Write семантикой.

## Архитектура

```
┌─────────────────────────────────────────────────────┐
│                    User Space                        │
│  ┌──────────────────┐  ┌──────────────────────────┐ │
│  │  WinKSM Service  │  │   Web Dashboard (:8787)  │ │
│  │  (HTTP API)      │──│   (HTML/CSS/JS)          │ │
│  └────────┬─────────┘  └──────────────────────────┘ │
│           │ IOCTL                                    │
├───────────┼──────────────────────────────────────────┤
│           │ Kernel Space                             │
│  ┌────────▼─────────────────────────────────────┐   │
│  │           WinKSM Driver (.sys)                │   │
│  │  ┌──────────┐ ┌──────────┐ ┌──────────┐     │   │
│  │  │ Scanner  │ │  Trees   │ │  Sharer  │     │   │
│  │  │ (xxhash) │ │(rb-stable│ │(section  │     │   │
│  │  │          │ │ rb-unstab│ │ objects) │     │   │
│  │  └──────────┘ └──────────┘ └──────────┘     │   │
│  │  ┌──────────┐ ┌──────────┐                   │   │
│  │  │  Rmap    │ │  Stats   │                   │   │
│  │  └──────────┘ └──────────┘                   │   │
│  └──────────────────────────────────────────────┘   │
│           │                                         │
├───────────┼──────────────────────────────────────────┤
│           ▼ Windows Memory Manager                   │
│  Section Objects → COW via Page Fault Handler        │
└─────────────────────────────────────────────────────┘
```

## Как это работает

1. **Сканирование**: Драйвер перебирает все процессы, читает их память через `ZwReadVirtualMemory`, хеширует каждую 4KB страницу через xxhash32.

2. **Двухдеревьевая архитектура** (как в Linux KSM):
   - **Stable tree**: Страницы, которые уже объединены. Никогда не перестраивается.
   - **Unstable tree**: Кандидаты на объединение. Перестраивается каждое полное сканирование.

3. **Объединение**: Когда две одинаковые страницы найдены, драйвер создаёт section object через `ZwCreateSection`, маппит его в целевой процесс через `ZwMapViewOfSection` на тот же виртуальный адрес, но с `PAGE_READONLY`. Windows Memory Manager автоматически обеспечивает COW при записи.

4. **Reverse Mapping**: Собственная реализация rmap для отслеживания какие процессы маппят какие страницы (Windows не имеет встроенного rmap).

## Сборка

### Требования
- Visual Studio 2022
- Windows Driver Kit (WDK)
- CMake 3.20+
- Тестовый режим подписи (для разработки)

### Сборка проекта
```powershell
# PowerShell
.\tools\build.ps1

# Или вручную:
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

### Установка
```batch
REM Запуск от администратора
tools\install.bat
```

### Ручная установка
```batch
REM 1. Включить тестовый режим
bcdedit /set testsigning on

REM 2. Установить драйвер
sc create winksm type= kernel binPath= "build\bin\Release\winksm.sys"
sc start winksm

REM 3. Запустить сервис
build\bin\Release\WinKSM_Service.exe --console
```

## Дашборд

Дашборд доступен по адресу: **http://localhost:8787**

- Мониторинг в реальном времени (обновление каждые 3 секунды)
- Включение/выключение KSM
- Настройка параметров сканирования
- Графики savings в реальном времени
- Статистика по всем процессам

## Параметры

| Параметр | По умолчанию | Описание |
|---|---|---|
| `pages_to_scan` | 100 | Страниц за один батч сканирования |
| `sleep_millisecs` | 20 | Пауза между батчами (мс) |
| `use_zero_pages` | false | Объединять нулевые страницы |
| `smart_scan` | true | Пропускать volatile страницы |
| `max_page_sharing` | 256 | Макс. число процессов на одну страницу |

## IOCTL Интерфейс

| Код | Назначение |
|---|---|
| `IOCTL_WINKSM_GET_STATUS` | Получить статус KSM |
| `IOCTL_WINKSM_SET_PARAMETERS` | Установить параметры |
| `IOCTL_WINKSM_START_SCAN` | Запустить сканирование |
| `IOCTL_WINKSM_STOP_SCAN` | Остановить сканирование |
| `IOCTL_WINKSM_GET_STATS` | Получить статистику |
| `IOCTL_WINKSM_ADD_TRUSTED_PID` | Добавить PID в белый список |
| `IOCTL_WINKSM_REMOVE_TRUSTED_PID` | Удалить PID из белого списка |

## Структура проекта

```
winksm/
├── driver/                  # Kernel-mode driver
│   ├── winksm_driver.c      # Точка входа, IOCTL dispatch
│   ├── winksm_scanner.c     # Движок сканирования страниц
│   ├── winksm_hasher.c      # xxhash32
│   ├── winksm_trees.c       # Red-black trees (stable/unstable)
│   ├── winksm_sharer.c      # Section objects для шаринга
│   ├── winksm_rmap.c        # Reverse mapping
│   ├── winksm_stats.c       # Сбор статистики
│   └── winksm.h             # Внутренние заголовки
│
├── service/                 # User-mode service
│   ├── main.cpp             # Service entry + IOCTL communication
│   └── dashboard.h          # Embedded HTTP server
│
├── shared/                  # Общие заголовки
│   ├── winksm_types.h       # Типы данных
│   └── winksm_protocol.h    # IOCTL коды и протокол
│
├── web/                     # Веб-дашборд
│   ├── index.html
│   ├── style.css
│   └── app.js
│
├── tools/                   # Скрипты сборки и установки
│   ├── build.ps1
│   ├── install.bat
│   └── sign.bat
│
└── CMakeLists.txt           # Top-level build
```

## Безопасность

- Драйвер использует только легитимные kernel APIs (PatchGuard-safe)
- Шаринг через section objects (не прямая модификация PTE)
- IOCTL доступен только администраторам
- Дашборд доступен только на localhost
- Опциональные trust groups для ограничения шаринга

## Лицензия

MIT License
