# Уязвимости KSM и как MemoryGuard их закрывает

Документ для ревью безопасности перед продакшеном. Отвечает на три вопроса:
какие атаки на KSM известны, что MemoryGuard делает против каждой, и что
закрыть в userspace **невозможно** (и поэтому решается политикой развёртывания).

Главный принцип, зафиксированный в документации ядра (2026):
**«любая память, помеченная mergeable, читаема любым другим процессом, использующим KSM»**.
KSM должен объединять только взаимно доверяющие нагрузки. Всё остальное в
MemoryGuard — уменьшение поверхности атаки внутри этой границы.

---

## 1. Реестр известных атак

| # | Атака / CVE | Год | Что даёт атакующему | Нужен код на хосте? |
|---|---|---|---|---|
| A1 | **CoW write-timing** (базовый канал), CVE-2015-2877, CAIN | 2011–2015 | Узнать, есть ли у жертвы страница с заданным содержимым; обход ASLR соседа | Да |
| A2 | **CVE-2024-0564** — `max_page_sharing` unmap timing (ядра 4.4 – 5.15.0-58, CVSS 6.5) | 2024 | Утечка содержимого страницы через время `munmap` при переполнении цепочки stable-node | Да |
| A3 | **Flip Feng Shui** (Rowhammer + KSM) | 2016 | Не утечка, а **запись**: дедуп сажает чужую страницу на уязвимую строку DRAM → bit-flip в RSA-ключе / `authorized_keys` жертвы | Да |
| A4 | **Remote dedup** (Schwarzl et al., NDSS 2022) | 2022 | Утечка данных Memcached / InnoDB / MariaDB **по сети**, только HTTP-запросами, ~34 B/ч | **Нет** |
| A5 | Covert channel через merge/unmerge | 2013+ | Скрытая передача данных между изолированными контейнерами | Да (оба конца) |
| A6 | Fingerprinting приложений / версий библиотек | 2013+ | Узнать ПО и версии соседа (а в A4 — удалённо: версия libc за ~166 с) | Да / Нет |
| A7 | FLUSH+RELOAD по общим страницам | 2014 | Трассировка исполнения жертвы через общий кеш | Да |

Ключевые факты, которые меняют модель угроз:

* **Дерево KSM одно на весь хост** (на NUMA-узел при `merge_across_nodes=0`).
  Нет «доменов слияния»: все mergeable-страницы всех процессов сравниваются со всеми.
  Upstream-ядро не умеет per-cgroup KSM (есть только патч в openEuler `memory.ksm`
  и патч per-memcg статистики, 2025).
* **A4 обходит whitelist контейнеров.** Атакующему не нужен свой контейнер на хосте —
  он кладёт данные в память *доверенного* Redis/Memcached обычными запросами.
* **Фильтр по имени библиотеки почти ничего не защищает** (подробно в п. 3.1).

---

## 2. Что было не так в MemoryGuard до этой ревизии

| Проблема | Где | Последствие |
|---|---|---|
| Blocklist `libcrypto/libssl/...` проверял file-backed маппинги, которые инжектор и так пропускал | `madvise_injector.c` | Фильтр был no-op; `.bss` криптобиблиотек и все heap-секреты мержились |
| Стеки потоков (не `[stack]`) мержились | `madvise_injector.c` | Локальные переменные (ключи, пароли) рабочих потоков JVM/Go/.NET в пуле KSM |
| ptrace останавливал только главный поток, затем патчил код по RIP | `madvise_injector.c` | Другие потоки могли выполнить подменённые байты → падения многопоточных сервисов |
| Гонка PID reuse между `docker top` и `PTRACE_ATTACH` | `Main.java` / инжектор | Можно было пометить процесс **недоверенного** контейнера |
| Periodic unmerge никогда не вызывался (`unmergeCountdown` не уменьшался) | `KsmTuner.java` | Защита из SECURITY.md (модуль 3) не работала |
| Emergency unmerge при аномалии не был реализован — только лог | `KsmTuner.java` | Защита из SECURITY.md (модуль 5) не работала |
| `max_page_sharing=2` молча не применялся, если страницы уже смержены (EBUSY) | `KsmController.java` | Mitigation CVE-2024-0564 оставался на 256, агент писал «OK» |
| `run=2` без проверки памяти | `KsmTuner.java` | На хосте, переподписанном благодаря KSM, unmerge = OOM-killer |
| Whitelist по `contains()` | `DockerClient.java` | Контейнер `evil-redis-x` проходил whitelist `redis` |
| API без аутентификации, `0.0.0.0`, ошибки без экранирования, нет лимитов | `ApiHandlers.java` | Любой (и CSRF из браузера админа) мог выключить KSM или выставить `sleep_millisecs=0` |

---

## 3. Решения (реализовано)

### 3.1. «Не сканировать определённые библиотеки» — как это работает на самом деле

KSM работает только с **анонимной** памятью. Код и read-only данные библиотек
(file-backed) он не трогает — их и так делит page cache. Секреты лежат не «в библиотеке»,
а в анонимной памяти, которую библиотека выделила. Поэтому фильтр по имени
работает только для одного случая, а остальное закрывается другими признаками:

| Где секрет | Как распознаём | Покрытие |
|---|---|---|
| `.bss` криптобиблиотеки (глобальное состояние RNG, кеши сессий) | Анонимный регион, вплотную примыкающий к последнему сегменту `libcrypto.so` и т.п. | ✅ C/C++, Python, Ruby, PHP, .NET (системный OpenSSL) |
| Защищённые аллокаторы: OpenSSL secure heap, `sodium_malloc`, libgcrypt secmem, Vault | `VmFlags` в `/proc/PID/smaps`: `lo` (mlock), `dd` (MADV_DONTDUMP) | ✅ любые языки, если приложение их использует |
| Состояние RNG / DRBG | `VmFlags: wf` (MADV_WIPEONFORK) | ✅ glibc ≥ 2.36 arc4random, OpenSSL 3 |
| Стеки всех потоков | RSP каждого потока после остановки всей группы потоков | ✅ |
| Ключи в общем heap (`malloc`, Java heap, Go heap) | **Нельзя** отличить от обычных данных извне процесса | ❌ см. 4 |

**Важно для стека бенчмарка:** Go (crypto/tls), Java (JCA/JSSE) и Node.js
(OpenSSL статически внутри бинарника `node`) не грузят `libssl.so` — для них фильтр по
библиотеке ничего не видит. Их защита — только политика контейнеров (3.3).

Доп. библиотеки: `MEMORYGUARD_EXCLUDE_LIBS=libmycrypto,libvendor_hsm`.
Проверка без изменений процесса: `madvise_injector --dry-run <pid>` печатает
классификацию каждого региона.

Проверено на ядре Docker Desktop (WSL2): `.bss` `libcrypto.so.3` → `sensitive-lib-bss`,
mlock / DONTDUMP / WIPEONFORK → `secret-vmflag`, 8 стеков потоков по 8 МБ →
`thread-stack`, процесс с 4 потоками в busy-loop пережил 5 инжекций и unmerge.

### 3.2. Безопасная инжекция

* Останавливаются **все** потоки (`/proc/PID/task/*`), повтор до стабилизации списка.
* Если поток стоит в syscall, используется уже существующая инструкция `syscall` по
  `RIP-2` — код процесса вообще не модифицируется. Иначе патч безопасен, т.к. все потоки стоят.
* `orig_rax = -1` — ядро не применяет логику рестарта syscall к инжектированному вызову.
* `--container <id>`: PID проверяется по `/proc/PID/cgroup` **до и после** attach,
  плюс сверка `starttime` из `/proc/PID/stat` → гонка PID reuse закрыта.
* Агент помнит процессы по паре `pid:starttime`, а не по PID.
* `--unmerge` — откат конкретного процесса через `MADV_UNMERGEABLE` (вместо глобального `run=2`).

### 3.3. Политика доверия (`TrustPolicy.java`)

Правила по порядку, первое совпадение решает:

1. label `memoryguard.ksm=false` → **запрет** (opt-out всегда сильнее)
2. label `memoryguard.domain` ≠ `MEMORYGUARD_TRUST_DOMAIN` → **запрет** (другой домен доверия)
3. образ — хранилище данных (`redis, memcached, valkey, mysql, mariadb, postgres, mongo, vault, etcd…`) → **запрет** — защита от A4
4. label `memoryguard.ksm=true` → разрешить, **только если** `MEMORYGUARD_TRUST_LABELS=true`
5. имя или образ совпадает с `MEMORYGUARD_CONTAINER_WHITELIST` (точно или glob `*`) → разрешить
6. иначе → запрет

Почему labels по умолчанию могут только *запрещать*: label ставит тот, кто запускает
контейнер. В multi-tenant это арендатор, т.е. потенциальный атакующий. Если бы
`memoryguard.ksm=true` включал слияние, атакующий сам добавил бы себя в пул KSM.
`MEMORYGUARD_TRUST_LABELS=true` включайте, только когда labels ставит платформа
(admission controller, CI).

> ⚠️ **Breaking change:** раньше `demo-adservice` совпадал с `demo-adservice-3` по подстроке.
> Теперь нужно `demo-adservice*`. `demo-redis-cart` теперь отклоняется правилом 3.

### 3.4. Hardening ядра с проверкой (fail-closed)

* `max_page_sharing=2`, `merge_across_nodes=0`: при EBUSY агент делает `run=2`
  (только если хватает памяти), меняет параметр, восстанавливает `run`, **перечитывает**
  значение. Проверено: с ядра 256 → 2 при уже смерженных страницах.
* Если hardening не подтверждён — инжекция **отключается** (`MEMORYGUARD_STRICT=false` вернёт старое поведение).

### 3.5. Реакция на атаку и periodic unmerge

* Periodic unmerge теперь действительно выполняется каждые 20–60 тиков (~5–15 мин).
* Аномалия → по умолчанию только алерт (`MEMORYGUARD_ANOMALY_ACTION=log`): в нагрузочном эксперименте 3
  детектор на глобальных счётчиках дважды сработал ложно и каждый раз останавливал экономию на 10+ минут.
  С `MEMORYGUARD_ANOMALY_ACTION=respond`: если памяти хватает — `run=2` + KSM в «медленном» режиме (`sleep=200ms`, `scan=50`);
  если не хватает: `run=0` — **заморозка** ksmd. Новые слияния не происходят, значит
  пробные страницы атакующего больше не сливаются с чужими, а уже полученная экономия сохраняется.
  Состояние держится ~10–13 мин, затем KSM возобновляется.
* Любой `run=2` проверяет запас: `MemAvailable − (pages_sharing + ksm_zero_pages) × 4K > 10% MemTotal`.
  Иначе — пропуск с логом «OOM risk».

### 3.6. API

* Без `MEMORYGUARD_API_TOKEN` управляющие эндпоинты отвечают 403 (только чтение).
* С токеном: `Authorization: Bearer <token>` (сравнение за константное время) +
  обязательный `Content-Type: application/json` → CSRF невозможен (нужен CORS preflight).
* Лимиты: `sleepMillisecs ∈ [10, 10000]`, `pagesToScan ∈ [1, 30000]`. Ошибки экранируются через Gson.
* `MEMORYGUARD_BIND_ADDRESS` (в контейнере публикуйте как `-p 127.0.0.1:8787:8787`).

---

## 4. Что НЕ решается в userspace — и что с этим делать

| Остаточный риск | Почему нельзя закрыть в MemoryGuard | Решение |
|---|---|---|
| Секреты в общем heap (Java/Go/.NET/Node) | Ядро видит страницу, а не объект; адрес ключа знает только сам процесс | Доверенный домен (3.3). Для приложения: хранить ключи в mlock/DONTDUMP-памяти или `memfd_secret` (не мержится) |
| CoW-timing внутри доверенного домена (A1, A5) | Это свойство KSM: merged-запись всегда медленнее | Один домен доверия на хост (выделенные node pool per tenant) |
| Rowhammer / Flip Feng Shui (A3) | Аппаратная проблема; KSM лишь даёт контроль над физическим размещением | ECC-память с TRR на хостах с KSM; не мержить страницы, целостность которых критична (публичные ключи, конфиги) |
| Remote dedup на whitelisted-сервисах (A4) | Данные приходят по сети в доверенный процесс | Правило 3 (data stores запрещены по умолчанию); не мержить сервисы, хранящие пользовательский ввод |
| `max_page_sharing=2` режет экономию | Mitigation A2 ограничивает страницу двумя владельцами | См. п. 5 |

**Изоляция доменов на NUMA:** при `merge_across_nodes=0` у каждого NUMA-узла своё дерево
KSM. На многосокетных хостах можно разнести домены доверия по узлам
(`cpuset.mems`) и получить настоящую изоляцию слияния без патча ядра.

**Уровень ядра (если нужно 100%):** patch openEuler `memory.ksm` (per-cgroup), исследовательские
VUsion / FakeDD (выравнивают время CoW). Upstream этого не содержит.

---

## 5. Компромисс экономии: измерено

Тест на ядре Docker Desktop, 8 МБ одинаковых страниц:

| `max_page_sharing` | Физических страниц | Экономия |
|---|---|---|
| 256 (по умолчанию ядра) | 19 на 4102 маппинга | **99.5%** |
| 2 (mitigation CVE-2024-0564) | 1024 на 2048 | **50%** (потолок) |

Следствие для статьи и бенчмарка: при `max_page_sharing=2` слияние **ненулевых**
одинаковых страниц не может дать больше 50%. Результат adservice (−89%) почти
наверняка получен на **нулевых** страницах: `-XX:+AlwaysPreTouch` заполняет heap нулями, а
`use_zero_pages=1` сливает их с нулевой страницей ядра в обход `max_page_sharing`.
Это хорошая новость: нулевые страницы не содержат секретов, и основная экономия
достаётся почти без риска. Но нужно перепроверить, а в статье честно разделить
«zero pages» и «настоящую дедупликацию» (`/proc/PID/ksm_stat`: `ksm_zero_pages` vs `ksm_merging_pages`).

На ядрах ≥ 5.15.0-59 (и дистрибутивных бэкпортах) CVE-2024-0564 исправлен, и
можно поднять `max_page_sharing` до 8–16. Это настраиваемый компромисс, а не константа.

---

## 6. Рекомендованный профиль для продакшена

```bash
docker run -d --name memoryguard \
  --pid=host --cap-add=SYS_PTRACE \
  -v /sys/kernel/mm/ksm:/sys/kernel/mm/ksm:rw \
  -v /var/run/docker.sock:/var/run/docker.sock \
  -p 127.0.0.1:8787:8787 \
  -e MEMORYGUARD_CONTAINER_WHITELIST='demo-adservice*,demo-frontend*' \
  -e MEMORYGUARD_TRUST_DOMAIN=shop-prod \
  -e MEMORYGUARD_API_TOKEN="$(openssl rand -hex 32)" \
  memoryguard:latest
```

* Вместо `--privileged` + `seccomp=unconfined` (полный root хоста при RCE в HTTP-сервере).
  ⚠️ Профиль с минимальными правами в этой ревизии **не проверялся**: нужно подтвердить
  запись в bind-mount sysfs и ptrace под AppArmor `docker-default` на целевом дистрибутиве.
* Docker socket даёт root на хосте. Используйте docker-socket-proxy с доступом только к
  `GET /containers/json` и `GET /containers/{id}/top`.
* Один домен доверия на хост. Арендаторы не смешиваются на нодах с KSM.
* Начинайте с `madvise_injector --dry-run` на каждом типе сервиса.

### Переменные окружения

| Переменная | По умолчанию | Назначение |
|---|---|---|
| `MEMORYGUARD_CONTAINER_WHITELIST` | — (deny all) | Точные имена/образы или glob с `*`; `*` = все |
| `MEMORYGUARD_TRUST_DOMAIN` | `default` | Домен доверия хоста; сравнивается с label `memoryguard.domain` |
| `MEMORYGUARD_TRUST_LABELS` | `false` | Разрешить label `memoryguard.ksm=true` включать слияние |
| `MEMORYGUARD_DENY_IMAGES` | redis, memcached, … | Образы, запрещённые без явного opt-in; пустая строка = нет запретов |
| `MEMORYGUARD_EXCLUDE_LIBS` | — | Доп. библиотеки, чей `.bss` не мержить |
| `MEMORYGUARD_STRICT` | `true` | Отключать инжекцию, если hardening не подтверждён |
| `MEMORYGUARD_PERIODIC_UNMERGE` | `true` | Периодический `run=2` (с проверкой памяти) |
| `MEMORYGUARD_API_TOKEN` | — (read-only) | Токен для POST `/api/ksm/*` |
| `MEMORYGUARD_BIND_ADDRESS` | `0.0.0.0` | Адрес HTTP-сервера |
| `MEMORYGUARD_EXCLUDE_CONTAINERS` (`MEMORYGUARD_BLACKLIST`) | — | Исключения: имена/образы точно или glob; сильнее whitelist |
| `MEMORYGUARD_MAX_PAGE_SHARING` | `2` | 2–256. 2 = сильнейшая защита от CVE-2024-0564, но потолок 50% на одинаковых ненулевых страницах |
| `MEMORYGUARD_ANOMALY_ACTION` | `log` | `log` — только алерт; `respond` — unmerge/заморозка ksmd при аномалии |
| `MEMORYGUARD_TUNER` | `adaptive` | `adaptive` — скорость ksmd по обратной связи (экономия на просканированную страницу); `threshold` — три фиксированных режима по свободной памяти |
| `MEMORYGUARD_KSM_AUTOSTART` | `false` | Включить KSM (`run=1`) после подтверждённого hardening; иначе — через дашборд/API |
| `MEMORYGUARD_REINJECT_MINUTES` | `10` | Повторная инжекция уже обработанных процессов (новые регионы после роста heap); `0` — выключить |

---

## Источники

* CVE-2024-0564 — [NVD](https://nvd.nist.gov/vuln/detail/CVE-2024-0564), [Red Hat](https://access.redhat.com/security/cve/CVE-2024-0564), [Ubuntu](https://ubuntu.com/security/CVE-2024-0564)
* CVE-2015-2877 (CAIN) — [Red Hat](https://access.redhat.com/security/cve/CVE-2015-2877)
* Flip Feng Shui, USENIX Security 2016 — [VUSec](https://vusec.net/projects/flip-feng-shui)
* Remote Memory-Deduplication Attacks, NDSS 2022 — [PDF](https://www.gruss.cc/files/remote_dedup.pdf)
* Kernel docs: KSM (`max_page_sharing`, `smart_scan`, `ksm_stat`, `PR_SET_MEMORY_MERGE`) — [docs.kernel.org](https://docs.kernel.org/admin-guide/mm/ksm.html), [LWN overview](https://lwn.net/Articles/953141)
* Патч документации side-channel KSM, LKML 2026 — [lkml.iu.edu](https://lkml.iu.edu/2607.0/01477.html)
* Per-cgroup KSM (openEuler) — [patch](https://mailweb.openeuler.org/archives/list/kernel@openeuler.org/thread/WQWCSNBQ7T7HUCS2D5LK7V4ZXUOIE5IW/); per-memcg KSM stats — [LWN](https://lwn.net/Articles/1038957)
* `PR_SET_MEMORY_MERGE` наследуется через fork/exec — [LKML test](https://lkml.iu.edu/hypermail/linux/kernel/2309.2/04734.html)
