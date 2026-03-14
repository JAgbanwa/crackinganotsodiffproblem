# Makefile for s3ceq worker

CC      = gcc
CFLAGS  = -O3 -march=native -Wall -Wextra -std=c11 -lm
TARGET  = worker_s3ceq

.PHONY: all clean test

all: $(TARGET)

$(TARGET): worker.c
	$(CC) $(CFLAGS) -o $(TARGET) worker.c
	@echo "Built $(TARGET)"

# Quick smoke test: search n in [-100, 100]
test: $(TARGET)
	@echo "0 100" > /tmp/wu_test.txt
	./$(TARGET) /tmp/wu_test.txt /tmp/out_test.txt
	@echo "--- Output (n=0..100) ---"
	@cat /tmp/out_test.txt
	@echo "0 100" > /tmp/wu_test2.txt
	@echo "-100 -1" > /tmp/wu_neg.txt
	./$(TARGET) /tmp/wu_neg.txt /tmp/out_neg.txt
	@echo "--- Output (n=-100..-1) ---"
	@cat /tmp/out_neg.txt
	@echo "Smoke test done."

clean:
	rm -f $(TARGET) /tmp/wu_test.txt /tmp/out_test.txt worker_progress.log
