# syntax=docker/dockerfile:1.7

# ---------- Stage 1: build the frontend ----------
FROM node:22-alpine AS frontend
WORKDIR /app
COPY frontend_app/package*.json ./
RUN npm ci --no-audit --no-fund
COPY frontend_app/ ./
RUN npm run build

# ---------- Stage 2: build the C++ backend ----------
# Uses the official Drogon image (Ubuntu 22.04 + libdrogon prebuilt under
# /usr/local). Apt's libdrogon-dev in Ubuntu noble is only 1.8.7, which lacks
# API we use; this base sidesteps that problem.
FROM drogonframework/drogon:latest AS backend
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential pkg-config \
        libsodium-dev libcurl4-openssl-dev libssl-dev libbrotli-dev \
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
# Match the build base (Ubuntu 22.04) so libstdc++ matches; libdrogon is
# statically linked, so we only need the third-party shared deps.
FROM ubuntu:22.04 AS runtime
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libsodium23 libcurl4 libssl3 \
        libbrotli1 libjsoncpp25 libpq5 \
        libuuid1 zlib1g libc-ares2 \
        libmariadb3 libsqlite3-0 libhiredis0.14 \
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
