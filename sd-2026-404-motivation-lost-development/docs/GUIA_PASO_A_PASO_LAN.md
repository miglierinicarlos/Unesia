# Guía paso a paso (demo en 2 PCs por LAN)

Esta guía explica cómo reproducir el escenario:
**una PC registra nodos y otra PC puede verlos**.

---

## 1) Preparar la PC servidor (PC-A)

1. Entrar al proyecto:
   ```bash
   cd sd-2026-404-motivation-lost-development
   ```
2. Compilar:
   ```bash
   cmake -S . -B build \
     -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
     -DCMAKE_BUILD_TYPE=Release

   cmake --build build -j"$(nproc)"
   ```
3. Obtener la IP LAN de PC-A:
   ```bash
   hostname -I
   ```
4. Abrir el puerto del servidor (si usás UFW):
   ```bash
   sudo ufw allow 9026/tcp
   ```
5. Levantar el servidor:
   ```bash
   EOP_PORT=9026 ./build/exodus_app
   ```

> Dejá esta terminal corriendo.

---

## 2) Preparar la PC cliente 1 (PC-B)

1. Asegurar conectividad con PC-A:
   ```bash
   ping <IP_DE_PC_A>
   ```
2. Conectar cliente a la IP de PC-A (NO usar `127.0.0.1`):
   ```c
   eop_client_t* c1 = eop_connect("<IP_DE_PC_A>", 9026, 5000);
   ```
3. Registrar nodo:
   ```c
   eop_send_command(c1, EOP_REGISTER, ...);
   ```

---

## 3) Preparar la PC cliente 2 (PC-C)

1. Conectar también a la misma IP/puerto de PC-A:
   ```c
   eop_client_t* c2 = eop_connect("<IP_DE_PC_A>", 9026, 5000);
   ```
2. Consultar nodos:
   ```c
   eop_send_command(c2, EOP_QUERY, ...);
   ```

Si todo está bien, PC-C verá nodos registrados desde PC-B
(porque ambos usan el mismo servidor central en PC-A).

---

## 4) Errores típicos y solución rápida

- **`Connection refused`**: servidor no está corriendo o puerto incorrecto.
- **`Connection timed out`**: firewall o red bloqueando puerto `9026`.
- **No aparecen nodos en otra PC**: los clientes están conectando a servidores distintos.
- **Usar `127.0.0.1` en cliente remoto**: esto siempre apunta a la PC local del cliente.

---

## 5) Checklist final

- [ ] PC-A con `./build/exodus_app` activo.
- [ ] Clientes usando `eop_connect("<IP_DE_PC_A>", 9026, ...)`.
- [ ] Puerto `9026/tcp` accesible entre PCs.
- [ ] Mismo segmento de red o VPN entre máquinas.
