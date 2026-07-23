# API MemoryGuard

Встроенный HTTP REST API доступен на порту **8787**.

Базовый URL: `http://localhost:8787`

---

## Эндпоинты

### GET /api/status

Возвращает полный снимок состояния системы: статус KSM, метрики памяти, информацию о контейнерах и расчёт экономии.

**Запрос:**

```bash
curl -s http://localhost:8787/api/status | python3 -m json.tool
```

**Ответ:**

```json
{
    "ksmSupported": true,
    "ksmRunning": true,
    "pagesShared": 1024,
    "pagesSharing": 8192,
    "pagesUnshared": 4096,
    "pagesVolatile": 32,
    "fullScans": 15,
    "sleepMillisecs": 20,
    "pagesToScan": 200,
    "useZeroPages": true,
    "autoTune": true,
    "systemTotalMemory": 17179869184,
    "systemAvailableMemory": 8589934592,
    "systemMemoryUsagePercent": 50.0,
    "savedMemoryMb": 32.0,
    "monthlySavingsUsd": 0.09,
    "containers": [
        {
            "id": "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2",
            "name": "nginx-web",
            "image": "nginx:latest",
            "memoryUsageBytes": 52428800
        }
    ]
}
```

**Поля ответа:**

| Поле | Тип | Описание |
|---|---|---|
| `ksmSupported` | boolean | Поддерживает ли ядро KSM (`/sys/kernel/mm/ksm` существует) |
| `ksmRunning` | boolean | Работает ли демон ksmd (`run = 1`) |
| `pagesShared` | int | Физические страницы, на которые ссылаются ≥2 виртуальные (уникальные shared-страницы) |
| `pagesSharing` | int | Виртуальные страницы, ссылающиеся на shared-страницы (общее количество «схлопнутых» ссылок) |
| `pagesUnshared` | int | Просканированные страницы, для которых не нашлось дубликатов |
| `pagesVolatile` | int | Страницы, изменившиеся до завершения сканирования (CoW во время скана) |
| `fullScans` | int | Полных проходов ksmd по всей помеченной памяти |
| `sleepMillisecs` | int | Текущая пауза ksmd между проходами (миллисекунды) |
| `pagesToScan` | int | Количество страниц, проверяемых ksmd за один проход |
| `useZeroPages` | boolean | Мержить ли нулевые (пустые) страницы |
| `autoTune` | boolean | Включена ли автоматическая настройка параметров |
| `systemTotalMemory` | long | Общая RAM хоста (байты) |
| `systemAvailableMemory` | long | Доступная RAM хоста (байты) |
| `systemMemoryUsagePercent` | double | Процент использования RAM хоста |
| `savedMemoryMb` | double | Экономия RAM = `pagesSharing × 4096 / 1MB` |
| `monthlySavingsUsd` | double | Стоимость экономии = `savedGB × $3.00/GB/мес` |
| `containers` | array | Список Docker-контейнеров с информацией о RAM |

**Объект контейнера:**

| Поле | Тип | Описание |
|---|---|---|
| `id` | string | Полный Docker container ID (64 hex символа) |
| `name` | string | Имя контейнера (без `/`) |
| `image` | string | Имя Docker-образа |
| `memoryUsageBytes` | long | Потребление RAM из cgroup (байты) |

**Как считается экономия:**

```
Каждая shared-страница = 4096 байт (стандартный размер страницы Linux).
pages_sharing = общее число виртуальных страниц, экономящих физическую.
Экономия = pages_sharing × 4096 байт.

Пример: pages_sharing = 8192 → экономия = 32 MB.
Облачная стоимость: 32 MB = 0.03125 GB × $3/GB/мес = $0.09/мес.
```

---

### POST /api/ksm/toggle

Включает или выключает демон KSM (ksmd).

**Запрос:**

```bash
# Включить KSM
curl -X POST http://localhost:8787/api/ksm/toggle \
  -H "Content-Type: application/json" \
  -d '{"run": true}'

# Выключить KSM
curl -X POST http://localhost:8787/api/ksm/toggle \
  -H "Content-Type: application/json" \
  -d '{"run": false}'
```

**Тело запроса:**

| Поле | Тип | Обязательно | Описание |
|---|---|---|---|
| `run` | boolean | да | `true` = включить ksmd, `false` = выключить |

**Ответ (успех):**

```json
{"success": true}
```

**Ответ (ошибка):**

```json
{"error": "сообщение об ошибке"}
```

**Коды ответов:**

| Код | Описание |
|---|---|
| 200 | Операция выполнена |
| 400 | Ошибка в теле запроса (невалидный JSON) |
| 405 | Метод не POST |

---

### POST /api/ksm/tune

Изменяет параметры работы KSM. Все поля необязательны — можно отправить только нужные.

**Запрос:**

```bash
# Изменить все параметры
curl -X POST http://localhost:8787/api/ksm/tune \
  -H "Content-Type: application/json" \
  -d '{
    "autoTune": false,
    "pagesToScan": 500,
    "sleepMillisecs": 10,
    "useZeroPages": true
  }'

# Только выключить auto-tune
curl -X POST http://localhost:8787/api/ksm/tune \
  -H "Content-Type: application/json" \
  -d '{"autoTune": false}'
```

**Тело запроса:**

| Поле | Тип | Обязательно | Диапазон | Описание |
|---|---|---|---|---|
| `autoTune` | boolean | нет | — | Включить/выключить автонастройку параметров |
| `pagesToScan` | int | нет | 10–10000 | Страниц за проход ksmd. Больше = агрессивнее, больше CPU |
| `sleepMillisecs` | int | нет | 0–60000 | Пауза ksmd между проходами. Меньше = агрессивнее |
| `useZeroPages` | boolean | нет | — | Мержить нулевые страницы (безопасно, рекомендуется true) |

> **Примечание:** если `autoTune = true`, вручную установленные значения `pagesToScan` и `sleepMillisecs` будут перезаписаны тюнером на следующем цикле (через 12–20 сек).

**Ответ (успех):**

```json
{"success": true}
```

**Ответ (ошибка):**

```json
{"error": "сообщение об ошибке"}
```

---

### GET / (статические файлы)

Отдаёт встроенные файлы веб-дашборда из classpath.

| Путь | Файл |
|---|---|
| `/` | `index.html` |
| `/index.html` | `index.html` |
| `/style.css` | `style.css` |
| `/app.js` | `app.js` |

**Content-Type:**

| Расширение | Тип |
|---|---|
| `.html` | `text/html; charset=utf-8` |
| `.css` | `text/css; charset=utf-8` |
| `.js` | `application/javascript; charset=utf-8` |
| Прочее | `text/plain` |

---

## Коды ответов (общие)

| Код HTTP | Значение |
|---|---|
| 200 | Успешный запрос |
| 400 | Ошибка клиента (невалидный JSON, недопустимые параметры) |
| 404 | Ресурс не найден (статический файл) |
| 405 | Неподдерживаемый HTTP-метод (GET vs POST) |

---

## Примеры использования

### Мониторинг экономии в bash

```bash
# Каждые 10 сек показывать экономию
watch -n 10 "curl -s http://localhost:8787/api/status | \
  python3 -c \"import sys,json; d=json.load(sys.stdin); \
  print(f'Saved: {d[\\\"savedMemoryMb\\\"]:.1f} MB, \
  Cost: \${d[\\\"monthlySavingsUsd\\\"]:.2f}/mo, \
  Shared pages: {d[\\\"pagesSharing\\\"]}')\""
```

### Prometheus-совместимый скрипт

```bash
#!/bin/bash
DATA=$(curl -s http://localhost:8787/api/status)
echo "ksm_pages_sharing $(echo $DATA | python3 -c 'import sys,json; print(json.load(sys.stdin)["pagesSharing"])')"
echo "ksm_pages_shared $(echo $DATA | python3 -c 'import sys,json; print(json.load(sys.stdin)["pagesShared"])')"
echo "ksm_saved_mb $(echo $DATA | python3 -c 'import sys,json; print(json.load(sys.stdin)["savedMemoryMb"])')"
echo "ksm_system_usage_pct $(echo $DATA | python3 -c 'import sys,json; print(json.load(sys.stdin)["systemMemoryUsagePercent"])')"
```

### Интеграция с Grafana

Используйте Grafana `JSON API` datasource plugin, указав URL `http://memoryguard:8787/api/status`. Все поля ответа доступны как метрики.
