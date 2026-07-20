# Инструкция по запуску WinKSM

## Быстрый старт (5 минут)

### Шаг 1: Установить зависимости

| Программа | Где скачать | Версия |
|---|---|---|
| Visual Studio 2022 | https://visualstudio.microsoft.com/ | Community (бесплатная) |
| Windows Driver Kit (WDK) | https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk | Для Windows 10/11 |
| CMake | https://cmake.org/download/ | 3.20+ |

**При установке Visual Studio выбрать компоненты:**
- "Desktop development with C++"
- "Windows 10/11 SDK (10.0.19041.0)"

**При установке CMake:**
- Выбрать "Add CMake to system PATH"

### Шаг 2: Включить тестовый режим подписи

Драйвер без подписи не запустится. Для разработки нужен тестовый режим.

```batch
REM Запустить CMD от имени администратора
bcdedit /set testsigning on
```

**После этого ОБЯЗАТЕЛЬНО перезагрузить компьютер.**

### Шаг 3: Собрать проект

Открыть PowerShell в папке проекта:

```powershell
cd F:\projects\KMS\winksm
powershell -ExecutionPolicy Bypass -File tools\build.ps1
```

Или вручную:

```powershell
cd F:\projects\KMS\winksm
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

**Результат сборки:**
```
build\bin\Release\winksm.sys           ← драйвер ядра
build\bin\Release\WinKSM_Service.exe   ← пользовательский сервис
build\bin\Release\web\                  ← файлы дашборда
```

### Шаг 4: Установить и запустить

```batch
REM Запустить CMD от имени администратора
cd F:\projects\KMS\winksm
tools\install.bat
```

Или вручную:

```batch
REM 1. Установить драйвер
sc create winksm type= kernel binPath= "F:\projects\KMS\winksm\build\bin\Release\winksm.sys"
sc start winksm

REM 2. Запустить сервис (консольный режим для отладки)
F:\projects\KMS\winksm\build\bin\Release\WinKSM_Service.exe --console
```

### Шаг 5: Открыть дашборд

Открыть браузер: **http://localhost:8787**

На дашборде:
- Нажать "Enable KSM Engine" для запуска сканирования
- Настраивать параметры сканирования
- Смотреть графики в реальном времени

### Шаг 6: Остановить и удалить

```batch
REM Остановить сервис
sc stop WinKSM_Service
sc delete WinKSM_Service

REM Остановить драйвер
sc stop winksm
sc delete winksm

REM Отключить тестовый режим (опционально)
bcdedit /set testsigning off
```

---

## Установка как Windows Service (автозапуск)

```batch
REM Установить как сервис, запускается автоматически при загрузке
sc create WinKSM_Service type= own start= auto binPath= "F:\projects\KMS\winksm\build\bin\Release\WinKSM_Service.exe"
sc start WinKSM_Service
```

---

## Консольный режим vs Сервис

| Режим | Запуск | Когда использовать |
|---|---|---|
| Консольный | `WinKSM_Service.exe --console` | Отладка, тестирование |
| Сервис | `sc start WinKSM_Service` | Продакшн, автозапуск |

---

## Возможные ошибки

### "System error 58 has occurred"
**Причина:** Драйвер не подписан или тестовый режим не включён.
**Решение:** `bcdedit /set testsigning on` → перезагрузка.

### "System error 2 has occurred"
**Причина:** Файл winksm.sys не найден по указанному пути.
**Решение:** Проверить путь в команде `sc create`. Использовать абсолютный путь.

### "Access is denied"
**Причина:** CMD запущен не от администратора.
**Решение:** ПКМ на CMD → "Запуск от имени администратора".

### Дашборд не открывается
**Причина:** Сервис не запущен или порт 8787 занят.
**Решение:** Проверить статус: `sc query WinKSM_Service`. Проверить порт: `netstat -ano | findstr 8787`.

### Драйвер вызывает BSOD
**Причина:** Ошибка в коде драйвера.
**Решение:** Проверить дамп памяти (`C:\Windows\MEMORY.DMP`). Использовать WinDbg для анализа.

---

## Отладка

### Просмотр логов драйвера

Использовать **DebugView** (Sysinternals) с включённым kernel mode:
1. Скачать DebugView с https://learn.microsoft.com/en-us/sysinternals/downloads/debugview
2. Запустить от администратора
3. Menu → Capture → Capture Kernel
4. Запустить драйвер

### Проверка статуса

```batch
REM Проверить, установлен ли драйвер
sc query winksm

REM Проверить, запущен ли сервис
sc query WinKSM_Service

REM Проверить, слушает ли порт
netstat -ano | findstr 8787
```

---

## Отключение

```batch
REM Полное удаление
sc stop WinKSM_Service >nul 2>&1
sc delete WinKSM_Service >nul 2>&1
sc stop winksm >nul 2>&1
sc delete winksm >nul 2>&1
bcdedit /set testsigning off
```
