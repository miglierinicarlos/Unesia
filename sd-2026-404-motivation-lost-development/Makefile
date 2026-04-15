# Makefile

.PHONY: deploy-local down logs check-size validate-k8s test-k8s-recovery test-k8s-autorestart

EOP_PORT ?= 9026

# ----------------------------------------------------------------
# deploy-local → levanta las imagenes de docker
# ----------------------------------------------------------------
deploy-local:
	docker compose up --build -d
	@echo "Esperando 10s a que ClickHouse encienda..."
	sleep 10
	@echo "Creando bases de datos de SigNoz (Traces y Logs)..."
	docker compose exec clickhouse clickhouse-client --query "CREATE DATABASE IF NOT EXISTS signoz_traces;"
	docker compose exec clickhouse clickhouse-client --query "CREATE DATABASE IF NOT EXISTS signoz_logs;"
	@echo "Inicializando control de migraciones..."
	docker compose run --rm otel-collector migrate bootstrap --clickhouse-dsn="tcp://signoz:signoz@clickhouse:9000" || true
	@echo "Construyendo las tablas de telemetría..."
	docker compose run --rm otel-collector migrate sync up --clickhouse-dsn="tcp://signoz:signoz@clickhouse:9000"
	@echo "Reiniciando el colector para aplicar cambios..."
	docker compose restart otel-collector
	@echo "Waiting for server to accept connections..."
	@timeout 60 bash -c \
	   'until bash -c "echo > /dev/tcp/localhost/$(EOP_PORT)" 2>/dev/null; \
	   do echo "  still waiting..."; sleep 2; done' \
	   || (echo "ERROR: server did not come up in 60s" && docker compose logs eop-server && exit 1)
	@echo " Smoke test passed, server accepting connections on port $(EOP_PORT)"

down:
	docker compose down --remove-orphans

logs:
	docker compose logs -f eop-server

# ----------------------------------------------------------------
# Verificar que la imagen runtime sea ≤ 50 MB
# Ejecutar después de: docker compose build
# ----------------------------------------------------------------
check-size:
	@IMAGE_ID=$$(docker compose images -q eop-server); \
	if [ -z "$$IMAGE_ID" ]; then \
		echo "Imagen no encontrada para servicio eop-server. Corré 'docker compose build eop-server' primero."; exit 1; \
	fi; \
	SIZE=$$(docker image inspect $$IMAGE_ID --format='{{.Size}}'); \
	MB=$$((SIZE / 1048576)); \
	echo "Image size: $${MB} MB"; \
	if [ $$MB -gt 50 ]; then echo "ERROR: imagen supera 50 MB (ADR-006)"; exit 1; fi; \
	echo "Image size OK"


# ----------------------------------------------------------------
# Validamos manifests K8s
# ----------------------------------------------------------------
validate-k8s:
	@which kubeconform > /dev/null 2>&1 \
		|| (echo "Instalar kubeconform: https://github.com/yannh/kubeconform" && exit 1)
	kubeconform -strict k8s/
	@echo "K8s manifests válidos"

# ----------------------------------------------------------------
# test-k8s-recovery → end-to-end pod restart and client recovery
# Requires a live cluster with the EOP server deployed (k8s/).
# AC Task 5 US-109: deletes the pod, waits for restart, verifies
# reconnect and node registration succeed end-to-end.
# ----------------------------------------------------------------
test-k8s-recovery:
	@bash tests/integration/pod_restart_recovery_flow.sh

# ----------------------------------------------------------------
# test-k8s-autorestart → validate automatic pod restart after crash
# Requires a live cluster with the EOP server deployed (k8s/).
# AC Task 6 US-109: kills PID 1 in the container, verifies
# restartPolicy=Always restarts it and pod returns to Ready state.
# Runs the crash cycle twice to prove reproducibility.
# ----------------------------------------------------------------
test-k8s-autorestart:
	@bash tests/integration/pod_auto_restart_test.sh
