# Plan: Process-Aware Memory Deduplication (PAMD)

## Проблема

Сейчас MemoryGuard помечает `MADV_MERGEABLE` **все** контейнеры на хосте. Ядро `ksmd` сканирует их все в **одном глобальном пуле** и merge'ит идентичные страницы между **любыми** процессами — включая разнородные сервисы (nginx + postgres + redis). Это:

1. **Бесполезно:** разные приложения имеют разное содержимое памяти → merge между ними минимален
2. **Опасно:** создаёт cross-application side-channel → атакующий из одного контейнера может проводить fingerprinting и timing-атаки на другой

## Идея PAMD

**Merge только между репликами одного и того же Docker-образа.** Если на хосте запущено 10 реплик `adservice:latest` — их JVM-память идентична на 89%, и merge между ними даёт огромную экономию. Но merge между `adservice` и `postgres` бессмыслен и только открывает attack surface.

### Принцип работы:
```
┌──────────────────────────────────────────────────────┐
│                    Хост Linux                        │
│                                                      │
│  Группа A: adservice:latest (10 реплик)              │
│  ┌────┐ ┌────┐ ┌────┐ ┌────┐ ... ┌────┐             │
│  │ R1 │ │ R2 │ │ R3 │ │ R4 │     │R10 │             │
│  └──┬─┘ └──┬─┘ └──┬─┘ └──┬─┘     └──┬─┘             │
│     │      │      │      │          │                │
│     └──────┴──────┴──────┴──────────┘                │
│              ✅ MADV_MERGEABLE                        │
│              (идентичная JVM-память → merge ~89%)     │
│                                                      │
│  Группа B: redis:7 (2 реплики)                       │
│  ┌────┐ ┌────┐                                       │
│  │ R1 │ │ R2 │   ✅ MADV_MERGEABLE (между собой)     │
│  └────┘ └────┘                                       │
│                                                      │
│  Синглтоны (1 реплика — merge не имеет смысла):      │
│  ┌──────────┐ ┌────────┐ ┌───────────────┐           │
│  │ frontend │ │loadgen │ │ memoryguard   │           │
│  └──────────┘ └────────┘ └───────────────┘           │
│  ❌ НЕ помечаются MADV_MERGEABLE                     │
│  (нет реплик → нечего мержить → только attack surface)│
└──────────────────────────────────────────────────────┘
```

### Почему это работает как защита:
- `adservice R1` может merge'иться только с `adservice R2..R10` (один и тот же код, одни и те же данные)
- `adservice` **не может** merge'иться с `redis` или `postgres` → cross-app side-channel закрыт
- Синглтоны (`frontend`, `loadgenerator`) вообще не участвуют в KSM → zero attack surface

---

## Архитектура изменений

### Файлы, которые нужно создать:

#### [NEW] `src/main/java/dev/memoryguard/containers/InjectionGroupManager.java`

Новый класс — ядро PAMD-логики. Отвечает за:

1. **Группировку контейнеров по базовому образу** (нормализация имени image: убираем тег/SHA, оставляем base name)
2. **Фильтрацию синглтонов** (группы с 1 контейнером — пропускаются)
3. **Предоставление API**: `getEligibleContainers()` — возвращает только контейнеры из групп ≥2 реплик

```java
public class InjectionGroupManager {
    
    // Нормализация имени образа: "gcr.io/boutique/adservice:v0.10.2" → "adservice"
    public static String normalizeImageName(String image);
    
    // Группировка контейнеров по нормализованному образу
    // Возвращает Map<String, List<ContainerInfo>> (ключ = normalized image)
    public Map<String, List<ContainerInfo>> groupByImage(List<ContainerInfo> containers);
    
    // Возвращает только контейнеры из групп с ≥ minReplicas репликами
    public List<ContainerInfo> getEligibleContainers(List<ContainerInfo> all, int minReplicas);
    
    // Логирование текущих групп для дашборда
    public void logGroups(Map<String, List<ContainerInfo>> groups);
}
```

**Алгоритм нормализации образа:**
```
"gcr.io/google-samples/microservices-demo/adservice:v0.10.2"
  → убираем registry prefix (gcr.io/google-samples/microservices-demo/)
  → убираем тег (:v0.10.2)
  → результат: "adservice"

"redis:7.2-alpine"
  → убираем тег (:7.2-alpine)  
  → результат: "redis"

"sha256:a1b2c3d4e5f6..."
  → используем как есть (контейнеры с одинаковым SHA = один образ)
```

---

### Файлы, которые нужно модифицировать:

#### [MODIFY] `src/main/java/dev/memoryguard/Main.java`

Изменить инжектор-цикл (строки 58–87):

**Было:**
```java
// Инжектируем ВСЕ доверенные контейнеры
for (DockerClient.ContainerInfo container : containers) {
    if (!dockerClient.isContainerTrusted(container)) continue;
    // inject...
}
```

**Станет:**
```java
InjectionGroupManager groupManager = new InjectionGroupManager();
String mode = System.getenv("MEMORYGUARD_GROUP_MODE"); // "image" | "off"

// Если PAMD включён (по умолчанию) — инжектируем только группы ≥2 реплик
List<ContainerInfo> eligible;
if ("off".equalsIgnoreCase(mode)) {
    eligible = containers; // старое поведение: все контейнеры
} else {
    eligible = groupManager.getEligibleContainers(containers, 2);
}

for (DockerClient.ContainerInfo container : eligible) {
    if (!dockerClient.isContainerTrusted(container)) continue;
    // inject...
}
```

---

#### [MODIFY] `src/main/java/dev/memoryguard/containers/DockerClient.java`

Добавить поле `imageId` в `ContainerInfo` для точной группировки (Docker API возвращает `ImageID` — SHA256 хеш образа, который одинаков у всех реплик одного deployment):

```java
public static class ContainerInfo {
    public String id;
    public String name;
    public String image;      // "adservice:v0.10.2" (человекочитаемое)
    public String imageId;    // "sha256:a1b2c3..." (точный хеш для группировки)
    public long memoryUsageBytes;
}
```

В методе `getRunningContainers()` добавить парсинг поля `ImageID` из Docker API response.

---

#### [MODIFY] Web Dashboard

Добавить в дашборд отображение групп:
- В таблице контейнеров — колонка «Группа PAMD» (имя нормализованного образа)
- Визуальная индикация: контейнеры одной группы подсвечены одним цветом
- В API `/api/status` — новое поле `injectionGroups` с массивом групп и их реплик

---

### Новая переменная окружения

| Переменная | Значение по умолчанию | Описание |
|---|---|---|
| `MEMORYGUARD_GROUP_MODE` | `image` | Режим группировки: `image` (PAMD по образу), `off` (старое поведение — все контейнеры) |
| `MEMORYGUARD_MIN_REPLICAS` | `2` | Минимальное количество реплик в группе для включения оптимизации |

---

## Порядок реализации

### Фаза 1: InjectionGroupManager (ядро логики)
1. Создать класс `InjectionGroupManager.java`
2. Реализовать `normalizeImageName()` с обработкой registry prefix, тегов, SHA
3. Реализовать `groupByImage()` и `getEligibleContainers()`
4. Юнит-тесты нормализации

### Фаза 2: Интеграция в Main.java
1. Добавить `imageId` в `ContainerInfo` и парсинг из Docker API
2. Интегрировать `InjectionGroupManager` в инжектор-цикл
3. Добавить чтение `MEMORYGUARD_GROUP_MODE` и `MEMORYGUARD_MIN_REPLICAS`
4. Логирование: какие группы найдены, сколько реплик, какие пропущены

### Фаза 3: Обновление дашборда
1. Расширить `/api/status` полем `injectionGroups`
2. Добавить визуализацию групп в таблице контейнеров
3. Перевести новые элементы на русский

### Фаза 4: Документация
1. Обновить `SECURITY.md` — добавить раздел о PAMD
2. Обновить `DEPLOYMENT.md` — новые переменные окружения
3. Обновить `docker-compose.memoryguard.yml`

---

## Влияние на безопасность

| До PAMD | После PAMD |
|---|---|
| Все контейнеры в одном KSM-пуле | Только реплики одного образа мержатся |
| Cross-app fingerprinting возможен | Cross-app fingerprinting **невозможен** |
| Синглтоны бессмысленно помечены | Синглтоны полностью исключены |
| Атака: `redis` → проба `postgres` через timing | Атака невозможна: разные образы не мержатся |

## Влияние на экономию RAM

Минимальное. В нашем бенчмарке 45 контейнеров:
- **89.4% экономии** пришлось на 10 реплик `adservice` (одинаковый образ → PAMD включит их)
- **~46%** на 4 реплики `productcatalog` (одинаковый образ → PAMD включит)
- Синглтоны (`frontend`, `loadgenerator`) давали **0% экономии** и так → PAMD правильно их исключит

**Итого: PAMD не снижает экономию RAM, но радикально сокращает attack surface.**
