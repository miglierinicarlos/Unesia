# Makefile

.PHONY: deploy-local down logs check-size validate-k8s k8s-apply k8s-hpc-job test-k8s-rolling-restart test-k8s-recovery test-k8s-autorestart test-tsan benchmark profile clean-profiling

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
# k8s-apply → validates and applies all k8s manifests, waits for Ready
# ----------------------------------------------------------------
k8s-apply: validate-k8s
	kubectl apply -f k8s/
	@echo "Waiting for all pods to reach Ready state (timeout: 180s)..."
	kubectl wait --for=condition=Ready pod -l 'app in (eop-server,eop-hpc-engine)' --timeout=180s
	@echo "All pods ready."
	kubectl get pods

# ----------------------------------------------------------------
# k8s-hpc-job → applies the HPC engine one-shot Job and waits for completion
# ----------------------------------------------------------------
k8s-hpc-job: validate-k8s
	kubectl delete job eop-hpc-engine-job --ignore-not-found
	kubectl apply -f k8s/hpc-engine-job.yaml
	@echo "Waiting for HPC engine job to complete (timeout: 300s)..."
	kubectl wait --for=condition=Complete job/eop-hpc-engine-job --timeout=300s
	@echo "Job completed."
	kubectl logs job/eop-hpc-engine-job

# ----------------------------------------------------------------
# test-k8s-rolling-restart → zero-downtime rolling restart with concurrent load
# Requires a live cluster with the full EOP stack deployed (make k8s-apply).
# AC7 US-204: triggers rollout restart while clients send load; asserts zero errors.
# ----------------------------------------------------------------
test-k8s-rolling-restart:
	@bash tests/integration/rolling_restart_test.sh

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


# ----------------------------------------------------------------
# benchmark → builds and runs the HPC benchmark suite
# ----------------------------------------------------------------
benchmark:
	@which cmake > /dev/null 2>&1 || (echo "cmake not found" && exit 1)
	cmake -S . -B build_release -G Ninja \
	   -DCMAKE_BUILD_TYPE=Release \
	   -DCMAKE_TOOLCHAIN_FILE=$(VCPKG_ROOT)/scripts/buildsystems/vcpkg.cmake \
	   -DCMAKE_C_COMPILER=clang \
	   -DCMAKE_CXX_COMPILER=clang++ \
	   -DCMAKE_CXX_FLAGS="-O3 -march=native" \
	   -DEXODUS_BUILD_BENCHMARKS=ON \
	   -DEXODUS_BUILD_TESTS=OFF
	cmake --build build_release --target benchmark_hpc --parallel
	mkdir -p docs/benchmark
	./build_release/benchmark_hpc | tee docs/benchmark/benchmark_report.md
	@echo "Report written to docs/benchmark/benchmark_report.md"

# ----------------------------------------------------------------
# profile → Runs the benchmark executable under Linux 'perf'
# This satisfies the architectural need for a sampling profiler.
# ----------------------------------------------------------------
profile:
	sudo sysctl -w kernel.perf_event_paranoid=-1
	@echo "Running sampling profiler (perf) on HPC engine..."
	@which perf > /dev/null 2>&1 || (echo "perf not found. Install linux-tools." && exit 1)
	perf record -F 99 -g -- ./build_release/benchmark_hpc
	@echo "Profiling complete. Run 'hotspot perf.data' to view the interactive CPU analysis."

# To verify concurrency correctness without compiler optimizations masking potential data races,
# ----------------------------------------------------------------
test-tsan:
	@which cmake > /dev/null 2>&1 || (echo "cmake not found" && exit 1)
	cmake -S . -B build_debug -G Ninja \
	   -DCMAKE_BUILD_TYPE=Debug \
	   -DCMAKE_TOOLCHAIN_FILE=$(VCPKG_ROOT)/scripts/buildsystems/vcpkg.cmake \
	   -DENABLE_TSAN=ON \
	   -DENABLE_ASAN=OFF \
	   -DEXODUS_BUILD_TESTS=ON \
	   -DEXODUS_BUILD_BENCHMARKS=OFF
	cmake --build build_debug --parallel
	cd build_debug && ctest -R "EngineConfig|ParallelDijkstra|ParallelComponents|BenchmarkHarness" --output-on-failure

# ----------------------------------------------------------------
# clean
# ----------------------------------------------------------------
clean-profiling:
	@echo "Remove profiling data and compilation folders"
	rm -rf build_release build_debug perf.data perf.data.old
	@echo "Environment clean!"
