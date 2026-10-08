CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror
LDLIBS = -lcurl
all: cloud-worker
cloud-worker: src/worker.c
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)
test: cloud-worker
	python3 -m unittest discover -s tests -v
clean:
	rm -f cloud-worker
.PHONY: all test clean
