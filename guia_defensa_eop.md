# 🛡️ Guía de Defensa — Exodus Ops Platform (EOP) v0.1

> **Proyecto**: Sistemas Distribuidos — Ingeniería en Computación, UNC  
> **Team**: Costamagna (Eng-B), Bejarano (Eng-A), Miglierini (Eng-C)

---

## ✅ Orden recomendado para la defensa

```
1. Levantar el stack       →  make deploy-local
2. Demostrar conectividad  →  cliente C conectándose al server
3. Ver trazas en SigNoz    →  http://localhost:8080
4. Correr tests unitarios  →  ctest
5. Tests de integración    →  integration_tests
6. Sanitizers (ASan/TSan)  →  mostrar "zero leaks / zero races"
7. Verificar tamaño imagen →  make check-size  (≤ 50 MB, ADR-006)
```

---

## 1️⃣ Levantar el stack completo (Docker Compose)

> Esto levanta: **eop-server + OTel Collector + SigNoz + ClickHouse + ZooKeeper**

```bash
cd /home/carlos/Documentos/DescargaSO2/sd-2026-404-motivation-lost-development

make deploy-local
```

**Esperar ~30 segundos** y verificar que todo está corriendo:

```bash
docker ps
```

Deberías ver contenedores para: `eop-server`, `signoz-otel-collector`, `signoz`, `clickhouse`, `zookeeper`.

Ver logs del server en tiempo real:

```bash
make logs
```

---

## 2️⃣ Probar la conectividad cliente → servidor

El server escucha en `localhost:9026`. Podés probarlo con el cliente C o con netcat:

### Con netcat (prueba rápida):
```bash
nc -z localhost 9026 && echo "✅ Puerto 9026 ABIERTO" || echo "❌ No responde"
```

### Con el cliente C (la demo real):
```c
// Ejemplo de flujo completo (del documento):

eop_client_t* client = eop_connect("127.0.0.1", 9026, 5000);

const char* payload = "{\"node_id\":\"vault-13\"}";
eop_response_t* resp = eop_send_command(
    client, EOP_REGISTER,
    (const uint8_t*)payload, strlen(payload));

if (resp && resp->msg_type == EOP_ACK) {
    printf("✅ Registered successfully\n");
}

eop_response_free(resp);
eop_disconnect(client);
```

---

## 3️⃣ Ver trazas en SigNoz (Observabilidad — ADR-005)

1. Abrir el navegador en: **http://localhost:8080**
2. Ir a la sección **"Traces"**
3. Filtrar por servicio: `eop-server`
4. Mostrar los spans que se emiten automáticamente:

| Operación | Span Name |
|-----------|-----------|
| Conexión de cliente | `connection_worker` |
| Registro de nodo | `register_node` |
| Consulta de nodo | `query_node` |
| Listar nodos | `list_nodes` |
| Heartbeat | `heartbeat_handler` |
| Desconexión | `node_offline` |

> **Punto clave para defender**: Los logs JSON incluyen `trace_id` y `span_id` para correlacionar logs con trazas. Mostrar un log del server y el trace correspondiente en SigNoz.

```bash
# Ver logs estructurados del server (con trace_id):
make logs
```

---

## 4️⃣ Tests Unitarios

```bash
# Compilar los tests (si no están compilados):
cmake --build build -j$(nproc) --target unit_tests

# Correr todos los tests:
ctest --test-dir build --output-on-failure
```

Cubren: `NodeRegistry`, handlers de protocolo, serialización, `ThreadPool`, `WorkQueue`.

**Objetivo en CI**: ≥ 90% coverage en lógica de negocio.

---

## 5️⃣ Tests de Integración (end-to-end)

```bash
# Terminal 1 — server corriendo:
./build/exodus_app &
SERVER_PID=$!

# Terminal 2 — correr integration tests:
./build/tests/integration_tests --gtest_output=xml

# Limpiar:
kill $SERVER_PID
```

**Suites clave a mencionar**:
- **Client lifecycle**: `connect → register → query → disconnect`
- **Concurrent stress**: 10 threads × 100 ciclos, zero leaks/races
- **Pod restart recovery** (Kubernetes): crash → reconnect → re-register

---

## 6️⃣ AddressSanitizer — Zero Memory Leaks (NFR-3)

```bash
cmake -S . -B build-asan \
    -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
    -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer"

cmake --build build-asan -j$(nproc)

./build-asan/tests/unit_tests
./build-asan/tests/eop_client_disconnect_test   # stress test del cliente
```

**Qué mostrar**: Que termina sin mensajes de `ERROR: AddressSanitizer`.

---

## 7️⃣ ThreadSanitizer — Zero Data Races (NFR-3)

```bash
cmake -S . -B build-tsan \
    -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=thread" \
    -DCMAKE_C_FLAGS="-fsanitize=thread"

cmake --build build-tsan -j$(nproc)
./build-tsan/tests/unit_tests
```

**Qué mostrar**: Que no hay `WARNING: ThreadSanitizer: data race`.

---

## 8️⃣ Verificar tamaño de imagen Docker (ADR-006)

```bash
make check-size
```

**Objetivo**: imagen runtime ≤ 50 MB (build multi-stage con imagen distroless).

---

## 9️⃣ Kubernetes (opcional / si les preguntan)

```bash
# Build de la imagen:
docker build -t eop-server:latest .

# Deploy al cluster:
kubectl apply -f k8s/

# Verificar pods:
kubectl get pods -l app=eop-server

# Lint de manifests:
make validate-k8s

# Test de recovery (pod restart + reconnect):
make test-k8s-recovery

# Test de auto-restart (matar PID 1):
make test-k8s-autorestart
```

---

## 🔑 Puntos clave para la defensa oral

| ADR | Qué explica |
|-----|-------------|
| ADR-001 | Por qué BSD/POSIX sockets (sin frameworks RPC) |
| ADR-002 | Modelo de concurrencia: Thread Pool |
| ADR-003 | Protocolo: length-prefix 4B + envelope 10B + JSON payload |
| ADR-005 | Observabilidad: OTel + SigNoz, logs con trace_id |
| ADR-006 | Docker multi-stage build distroless, imagen ≤ 50 MB |
| ADR-015 | Política de reconexión del cliente con exponential backoff |

### Protocolo de mensajes (saber de memoria):
| Tipo | ID | Dirección |
|------|----|-----------|
| REGISTER | 0x01 | Cliente → Server |
| ACK | 0x02 | Server → Cliente |
| ERROR | 0x03 | Server → Cliente |
| QUERY_NODE | 0x04 | Cliente → Server |
| LIST_NODES | 0x05 | Cliente → Server |
| HEARTBEAT | 0x06 | Cliente → Server |

---

## 🚀 Comando rápido para la demo

```bash
cd /home/carlos/Documentos/DescargaSO2/sd-2026-404-motivation-lost-development

# 1. Levantar todo
make deploy-local

# 2. Ver logs
make logs

# 3. (otra terminal) Correr tests
ctest --test-dir build --output-on-failure
```
