# Makefile for s3ceq worker (v2: Pollard-rho)

CC       = gcc
CFLAGS   = -O3 -march=native -Wall -Wextra -std=c11 -lm
# Portable Linux x86-64 flags (for cross-compile / BOINC deployment)
CFLAGS_LINUX = -O3 -march=x86-64 -Wall -Wextra -std=c11 -lm -static
TARGET   = worker_s3ceq
LINUX_BIN = worker_s3ceq_linux

.PHONY: all linux linux-docker clean test bench

# Build for the current machine (macOS / whatever host)
all: $(TARGET)

$(TARGET): worker.c
	$(CC) $(CFLAGS) -o $(TARGET) worker.c
	@echo "Built $(TARGET) (native)"

# Build a Linux x86-64 static binary (requires Linux or Docker — see linux-docker)
linux: worker.c
	$(CC) $(CFLAGS_LINUX) -o $(LINUX_BIN) worker.c
	@echo "Built $(LINUX_BIN) (linux-x86_64 static)"

# Build the Linux binary inside Docker (works on any host with Docker)
linux-docker: Dockerfile.worker
	@mkdir -p apps
	docker build -f Dockerfile.worker -t s3ceq-worker-builder .
	docker run --rm -v "$$(pwd)/apps:/out" s3ceq-worker-builder
	@echo "Linux binary: apps/worker_s3ceq_linux"

# Quick smoke test: search n in [-100, 100]
test: $(TARGET)
	@echo "0 100" > /tmp/wu_test.txt
	./$(TARGET) /tmp/wu_test.txt /tmp/out_test.txt
	@echo "--- Output (n=0..100) ---"
	@cat /tmp/out_test.txt
	@echo "-100 -1" > /tmp/wu_neg.txt
	./$(TARGET) /tmp/wu_neg.txt /tmp/out_neg.txt
	@echo "--- Output (n=-100..-1) ---"
	@cat /tmp/out_neg.txt
	@echo "Smoke test done."

# Speed benchmark: 100k values (should complete in < 5s with v2)
bench: $(TARGET)
	@echo "0 100000" > /tmp/wu_bench.txt
	@time ./$(TARGET) /tmp/wu_bench.txt /tmp/out_bench.txt
	@echo "Solutions found:"; cat /tmp/out_bench.txt

clean:
	rm -f $(TARGET) $(LINUX_BIN) /tmp/wu_*.txt /tmp/out_*.txt worker_progress.log
