# --- Этап 1: Сборка нативного бинарника в GraalVM ---
FROM ghcr.io/graalvm/native-image-community:21 AS builder

# Устанавливаем Maven внутри сборочного контейнера
RUN microdnf install -y maven

WORKDIR /build

# Копируем конфигурацию и исходный код
COPY pom.xml .
COPY src ./src

# Компилируем нативный исполняемый файл
RUN mvn clean package -Pnative

# --- Этап 2: Создание минимального рантайм-образа ---
FROM debian:bookworm-slim

# Устанавливаем procps для возможности работы с процессами (если потребуется ps)
RUN apt-get update && apt-get install -y procps && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Копируем скомпилированный нативный бинарник из первого этапа
COPY --from=builder /build/target/memoryguard /app/memoryguard

# Открываем порт для дашборда
EXPOSE 8787

# Запуск агента
ENTRYPOINT ["/app/memoryguard"]
