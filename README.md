# Unesia

Guía rápida para levantar y probar el proyecto principal de este repo:
`sd-2026-404-motivation-lost-development`.

## 1) Entrar al proyecto

```bash
cd sd-2026-404-motivation-lost-development
```

## 2) Requisitos mínimos

- CMake >= 3.28
- Compilador C++17 (GCC 12+ o Clang 15+)
- `vcpkg` instalado y variable `VCPKG_ROOT` configurada
- (Opcional) Docker + Docker Compose para correr todo el stack local

## 3) Compilar desde código fuente

```bash
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build -j"$(nproc)"
```

Binarios esperados:
- `build/exodus_app` (servidor)
- `build/client/libeop_client.so` y `build/client/libeop_client.a` (cliente)

## 4) Ejecutar el servidor

```bash
EOP_PORT=9026 EOP_THREAD_POOL_SIZE=16 EOP_MAX_CLIENTS=10000 \
EOP_HEARTBEAT_INTERVAL=5 EOP_IDLE_TIMEOUT=30 \
./build/exodus_app
```

## 5) Ejecutar pruebas

### Unit tests

```bash
cmake --build build -j"$(nproc)" --target unit_tests
ctest --test-dir build --output-on-failure
```

### Integration tests (requieren servidor corriendo)

```bash
./build/exodus_app &
SERVER_PID=$!

./build/tests/integration_tests --gtest_output=xml

kill "$SERVER_PID"
```

## 6) Alternativa rápida con Docker Compose

```bash
make deploy-local
make logs
make down
```

## 7) ¿Qué leo después?

- Guía completa del proyecto: `sd-2026-404-motivation-lost-development/README.md`
- Flujo de contribución/ramas: `sd-2026-404-motivation-lost-development/CONTRIBUTING.md`
