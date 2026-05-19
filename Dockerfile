# syntax=docker/dockerfile:1.7

# ---------- Stage 1: build the frontend ----------
FROM node:22-alpine AS frontend
WORKDIR /app
COPY frontend_app/package*.json ./
RUN npm ci --no-audit --no-fund
COPY frontend_app/ ./
RUN npm run build

# ---------- Stage 2: build the C++ backend ----------
FROM ubuntu:24.04 AS backend
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates build-essential cmake pkg-config git \
        libdrogon-dev libsodium-dev libcurl4-openssl-dev libssl-dev \
        libbrotli-dev libjsoncpp-dev uuid-dev zlib1g-dev \
        libpq-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt main.cc config.json ./
COPY controllers/ ./controllers/
COPY models/      ./models/
COPY helpers/     ./helpers/
COPY test/        ./test/
COPY schema.sql   ./schema.sql

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j"$(nproc)" --target blog

# ---------- Stage 3: runtime ----------
FROM ubuntu:24.04 AS runtime
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libdrogon1 libsodium23 libcurl4 libssl3 \
        libbrotli1 libjsoncpp25 libpq5 \
        postgresql-client \
        tini \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -m -u 1000 blog

WORKDIR /app
COPY --from=backend  /src/build/blog /app/blog
COPY --from=backend  /src/schema.sql /app/schema.sql
COPY --from=backend  /src/config.json /app/config.json
COPY --from=frontend /app/../public/ /app/public/
COPY docker/entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh && chown -R blog:blog /app && mkdir -p /app/uploads && chown blog:blog /app/uploads

USER blog
EXPOSE 8092
ENTRYPOINT ["/usr/bin/tini", "--", "/app/entrypoint.sh"]
