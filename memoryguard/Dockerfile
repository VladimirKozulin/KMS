# --- Этап 1: Сборка нативного бинарника в GraalVM ---
FROM ghcr.io/graalvm/native-image-community:21 AS builder

# Устанавливаем Maven внутри сборочного контейнера
RUN microdnf install -y maven

WORKDIR /build

# Копируем конфигурацию и исходный код
COPY pom.xml .
COPY src ./src

# Компилируем нативный исполняемый файл Java-агента
RUN mvn clean package -Pnative

# Компилируем C-хелпер madvise_injector
RUN gcc -O2 -o target/madvise_injector src/main/c/madvise_injector.c

# --- Этап 2: Создание минимального рантайм-образа ---
FROM debian:bookworm-slim

# Устанавливаем procps для работы с процессами
RUN apt-get update && apt-get install -y procps && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Копируем скомпилированные бинарники из первого этапа
COPY --from=builder /build/target/memoryguard /app/memoryguard
COPY --from=builder /build/target/madvise_injector /app/madvise_injector

# Открываем порт для дашборда
EXPOSE 8787

# Запуск агента
ENTRYPOINT ["/app/memoryguard"]
