# ======== STAGE 1: Builder ========
FROM debian:bookworm-slim AS builder

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential cmake && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN cmake -Bbuild -DCMAKE_BUILD_TYPE=Release && cmake --build build

# ======== STAGE 2: Runtime final ========
FROM debian:bookworm-slim AS runtime

# Copia somente o binário necessário
WORKDIR /app
COPY --from=builder /app/build/hello /app/hello

CMD ["./hello"]
