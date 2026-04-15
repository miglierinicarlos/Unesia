# ============================================================
# BUILD STAGE
# ============================================================
FROM debian:bookworm AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ninja-build \
    curl \
    zip \
    unzip \
    tar \
    pkg-config \
    git \
    ca-certificates \
    binutils \
    && rm -rf /var/lib/apt/lists/*

# CMake >= 3.28
RUN curl -fsSL https://github.com/Kitware/CMake/releases/download/v3.28.6/cmake-3.28.6-linux-x86_64.sh \
        -o /tmp/cmake-install.sh \
    && chmod +x /tmp/cmake-install.sh \
    && /tmp/cmake-install.sh --skip-license --prefix=/usr/local \
    && rm /tmp/cmake-install.sh

ENV VCPKG_ROOT=/opt/vcpkg

RUN git clone https://github.com/microsoft/vcpkg.git $VCPKG_ROOT \
    && $VCPKG_ROOT/bootstrap-vcpkg.sh -disableMetrics

WORKDIR /app

# ── Dependency layer (cached as long as vcpkg.json doesn't change) ──────────
COPY vcpkg.json .

# Credentials are injected via BuildKit secrets so their values do NOT affect
# the layer cache key — unlike ARG/ENV, secrets are invisible to the cache
# digest. Locally the secrets are absent and vcpkg compiles from source.
RUN --mount=type=secret,id=ACTIONS_CACHE_URL \
    --mount=type=secret,id=ACTIONS_RUNTIME_TOKEN \
    CACHE_URL=$(cat /run/secrets/ACTIONS_CACHE_URL 2>/dev/null || true) && \
    if [ -n "$CACHE_URL" ]; then \
        export ACTIONS_CACHE_URL="$CACHE_URL" && \
        export ACTIONS_RUNTIME_TOKEN=$(cat /run/secrets/ACTIONS_RUNTIME_TOKEN 2>/dev/null || true) && \
        export VCPKG_BINARY_SOURCES="clear;x-gha,readwrite"; \
    fi && \
    $VCPKG_ROOT/vcpkg install --triplet x64-linux --no-print-usage

# ── Source layer (only rebuilt when source changes) ──────────────────────────
COPY . .

RUN --mount=type=cache,target=/app/build \
    cmake -S . -B build \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
        -DEXODUS_BUILD_TESTS=OFF \
        -DEXODUS_BUILD_BENCHMARKS=OFF \
    && cmake --build build --target exodus_app \
    && mkdir -p /app/bin_out \
    && cp build/exodus_app /app/bin_out/exodus_app \
    && strip /app/bin_out/exodus_app

# ----------------------------------------------------------------------
RUN printf '%s\n' \
    '#include <arpa/inet.h>' \
    '#include <stdlib.h>' \
    '#include <sys/socket.h>' \
    '#include <unistd.h>' \
    'int main(void) {' \
    '    const char* p = getenv("EOP_PORT");' \
    '    const int port = p ? atoi(p) : 9026;' \
    '    const int s = socket(AF_INET, SOCK_STREAM, 0);' \
    '    if (s < 0) return 1;' \
    '    struct sockaddr_in a = {0};' \
    '    a.sin_family = AF_INET;' \
    '    a.sin_port = htons((unsigned short)port);' \
    '    a.sin_addr.s_addr = htonl(0x7f000001);' \
    '    const int rc = connect(s, (struct sockaddr*)&a, sizeof(a));' \
    '    close(s);' \
    '    return rc < 0 ? 1 : 0;' \
    '}' > /tmp/health.c \
    && gcc -Os -o /app/bin_out/eop_health /tmp/health.c \
    && rm /tmp/health.c

# ============================================================
# RUNTIME STAGE
# ============================================================
FROM gcr.io/distroless/cc-debian12:nonroot AS runtime

WORKDIR /app
COPY --from=builder /app/bin_out/exodus_app .
COPY --from=builder /app/bin_out/eop_health .
ENTRYPOINT ["./exodus_app"]
