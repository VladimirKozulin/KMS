# Контекст проекта MemoryGuard и история исследований (Для переноса в новый чат)

## 1. Суть проекта MemoryGuard
**MemoryGuard** — это автономный инфраструктурный демон/сайдкар-контейнер для сред Docker/Kubernetes, предназначенный для динамической оптимизации и дедупликации оперативной памяти (RAM) между работающими контейнерами на уровне хоста Linux.

* **Стек MemoryGuard:** Java 21 (GraalVM Native Image), C-инжектор (`madvise_injector.c`), REST API дашборд на порту `:8787`.
* **Запуск контейнера:**
  ```powershell
  docker run -d `
    --name memoryguard `
    --privileged `
    --pid=host `
    --cap-add=SYS_PTRACE `
    --security-opt seccomp=unconfined `
    -e MEMORYGUARD_CONTAINER_WHITELIST="demo-adservice,demo-cartservice,demo-frontend,demo-currencyservice,demo-recommendationservice,demo-emailservice,demo-paymentservice,demo-productcatalogservice,demo-checkoutservice,demo-shippingservice,demo-redis-cart,demo-loadgenerator" `
    -v /sys:/sys `
    -v /var/run/docker.sock:/var/run/docker.sock `
    -p 8787:8787 `
    memoryguard:latest
  ```

---

## 2. Проведенный стресс-тест и доказанная эффективность

### Тестовый стенд:
* Эталонный микросервисный комплекс **Google Online Boutique** (`docker-compose.bechmark.yml`) из 12 разнородных сервисов (Java, Go, .NET, Node.js, Python, Redis).
* Нагрузка подавалась через Locust (`demo-loadgenerator`).

### Результаты ступенчатого прогрева (Baseline):
* **10 пользователей:** RAM ~340 MB, CPU ~14%
* **100 пользователей:** RAM ~387 MB, CPU ~49% (18 RPS, 1982 запроса)
* **500 пользователей (Пик):** RAM 565.9 MB, CPU ~322% (91.3 RPS, 30 416 транзакций, 0.00% сбоев)

### Результаты замеров ДО и ПОСЛЕ включения MemoryGuard (на стабильных 500 пользователях):
| Микросервис / Контейнер | Стек / Язык | RAM ДО | RAM ПОСЛЕ | Чистая экономия |
|---|---|---|---|---|
| **demo-adservice** | Java 17 | 149.8 MB | **102.0 MB** | **-47.8 MB (-32.0%)** |
| **demo-frontend** | Go | 94.9 MB | **81.3 MB** | **-13.6 MB (-14.3%)** |
| **demo-cartservice** | .NET 7.0 | 89.8 MB | **84.5 MB** | **-5.3 MB (-5.9%)** |
| **demo-emailservice** | Python | 45.6 MB | **39.7 MB** | **-5.9 MB (-13.0%)** |
| **demo-productcatalogservice** | Go | 14.3 MB | **9.7 MB** | **-4.6 MB (-32.2%)** |
| **demo-shippingservice** | Go | 13.7 MB | **9.7 MB** | **-4.0 MB (-29.2%)** |
| **demo-checkoutservice** | Go | 14.1 MB | **12.3 MB** | **-1.8 MB (-12.8%)** |
| **demo-redis-cart** | C / Redis | 8.6 MB | **7.4 MB** | **-1.2 MB (-14.0%)** |
| **demo-currencyservice** | Node.js | 60.8 MB | **60.8 MB** | 0 MB |
| **demo-recommendationservice** | Python | 49.8 MB | **50.5 MB** | +0.7 MB |
| **demo-paymentservice** | Node.js | 24.7 MB | **27.5 MB** | +2.8 MB |
| **ИТОГО:** | | **565.9 MB** | **485.5 MB** | **-80.4 MB (-14.2%)** |

* **Оверхед самого MemoryGuard:** всего **0.39% CPU** и **54 MB RAM**.
* **Суммарный CPU сервисов:** снизился с **359.9% до 322.8%** (на **-37.1%**) за счет снижения GC-нагрузки.

---

## 3. Подготовленные материалы и статьи
1. [Исследование.md](file:///c:/Users/vladi/Desktop/CodeProjects/KMS/Исследование.md) — Полный технический протокол исследования с сырыми замерами.
2. [Статья_VC_Хабр_MemoryGuard.md](file:///c:/Users/vladi/Desktop/CodeProjects/KMS/Статья_VC_Хабр_MemoryGuard.md) — Готовая статья на русском для VC/Хабра с бизнес-позиционированием и скрытыми внутренними механизмами.
3. [LinkedIn_Article_MemoryGuard_EN.md](file:///c:/Users/vladi/Desktop/CodeProjects/KMS/LinkedIn_Article_MemoryGuard_EN.md) — Полная англоязычная статья для LinkedIn Pulse.
4. [LinkedIn_Post_Short_EN.md](file:///c:/Users/vladi/Desktop/CodeProjects/KMS/LinkedIn_Post_Short_EN.md) — Лаконичный пост для ленты LinkedIn (< 3000 символов) с хэштегами и контактами (@vakover).

---

## 4. Следующий запланированный шаг
* Подготовка масштабного визуального эксперимента **Часть 2**:
  - Масштабирование кластера микросервисов до **12–20 GB RAM** под нагрузкой **1500–3000 RPS**.
  - Подключение мониторинга **Prometheus + Grafana + cAdvisor** для получения наглядных графиков падения потребления памяти в реальном времени.
  - Публикация двухуровневой статьи (Visual Hook Post + Full Article) в LinkedIn и на Habr.
