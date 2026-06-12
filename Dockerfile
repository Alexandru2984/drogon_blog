# syntax=docker/dockerfile:1.7

# ---------- Stage 1: build the frontend ----------
FROM node:26-alpine AS frontend
WORKDIR /app
COPY frontend_app/package*.json ./
RUN npm ci --no-audit --no-fund
COPY frontend_app/ ./
RUN npm run build

# ---------- Stage 2: build libdrogon from source ----------
# The official drogonframework/drogon image only ships linux/amd64. Building
# Drogon ourselves on a multi-arch Ubuntu base lets the whole image cross to
# linux/arm64 (Hetzner CAX, Oracle Free Tier ARM, Raspberry Pi 4/5, …).
#
# Version pinned to 1.9.13 to match what the upstream image currently
# provides — the surface we depend on (registerSyncAdvice, runOnQuit,
# HttpResponsePtr-returning sync advice) landed in 1.9.x and isn't in the
# Ubuntu noble apt package (1.8.7).
FROM ubuntu:24.04 AS drogon-builder
ARG DEBIAN_FRONTEND=noninteractive
ARG DROGON_VERSION=v1.9.13
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates pkg-config \
        libssl-dev libjsoncpp-dev \
        libpq-dev libmariadb-dev libsqlite3-dev libhiredis-dev \
        libbrotli-dev zlib1g-dev uuid-dev libc-ares-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
RUN git clone --recurse-submodules --depth 1 --branch ${DROGON_VERSION} \
        https://github.com/drogonframework/drogon.git drogon
WORKDIR /src/drogon
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_DROGON_SHARED=OFF \
        -DBUILD_TESTING=OFF \
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_CTL=OFF \
 && cmake --build build -j"$(nproc)" \
 && cmake --install build

# ---------- Stage 3: build the C++ backend ----------
# Same base as the drogon-builder so libstdc++ ABI matches at link time;
# multi-arch via the Ubuntu base + Drogon-from-source above.
FROM ubuntu:24.04 AS backend
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential pkg-config cmake \
        libsodium-dev libcurl4-openssl-dev libssl-dev libbrotli-dev \
        libvips-dev \
        libcmark-gfm-dev libcmark-gfm-extensions-dev \
        libjsoncpp-dev libpq-dev libmariadb-dev libsqlite3-dev libhiredis-dev \
        zlib1g-dev uuid-dev libc-ares-dev \
    && rm -rf /var/lib/apt/lists/*

# libdrogon.a + headers (no shared libs — BUILD_DROGON_SHARED=OFF above).
COPY --from=drogon-builder /usr/local /usr/local

WORKDIR /src
COPY CMakeLists.txt main.cc config.json ./
COPY controllers/ ./controllers/
COPY models/      ./models/
COPY helpers/     ./helpers/
COPY test/        ./test/
COPY migrations/  ./migrations/
# openapi/ carries the spec + a self-hosted Redoc bundle that the
# api_docs handlers stream at /api/openapi.yaml and /api/docs.
COPY openapi/     ./openapi/

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j"$(nproc)" --target blog

# ---------- Stage 4: runtime ----------
# Match the build base (Ubuntu 24.04) so libstdc++ matches; libdrogon is
# statically linked, so we only need the third-party shared deps.
FROM ubuntu:24.04 AS runtime
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libsodium23 libcurl4t64 libssl3t64 \
        libbrotli1 libjsoncpp25 libpq5 \
        libuuid1 zlib1g libc-ares2 \
        libmariadb3 libsqlite3-0 libhiredis1.1.0 \
        libvips42t64 \
        libcmark-gfm0.29.0.gfm.6 libcmark-gfm-extensions0.29.0.gfm.6 \
        postgresql-client \
        tini \
    && rm -rf /var/lib/apt/lists/* \
    # noble ships a default "ubuntu" user squatting on UID 1000
    && userdel -r ubuntu \
    && useradd -m -u 1000 blog

WORKDIR /app
COPY --from=backend  /src/build/blog /app/blog
COPY --from=backend  /src/migrations /app/migrations
COPY --from=backend  /src/openapi    /app/openapi
COPY --from=backend  /src/config.json /app/config.json
COPY --from=frontend /app/../public/ /app/public/
COPY docker/entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh && chown -R blog:blog /app && mkdir -p /app/uploads && chown blog:blog /app/uploads

USER blog
EXPOSE 8092
ENTRYPOINT ["/usr/bin/tini", "--", "/app/entrypoint.sh"]
