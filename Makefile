CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror
LDLIBS = -lcurl -lcrypto
all: cloud-worker
cloud-worker: src/worker.c ps5/common/snapshot.c ps5/common/snapshot.h ps5/common/transfer.h
	$(CC) $(CFLAGS) src/worker.c ps5/common/snapshot.c -o $@ $(LDLIBS)
test: cloud-worker
	python3 -m unittest discover -s tests -v
clean:
	rm -f cloud-worker
.PHONY: all test clean
