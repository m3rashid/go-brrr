CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11

IMPL  = implementations
# latest implementation: what `make`, the Dockerfile, and docker-compose run.
# Update this (and the Dockerfile) when promoting a snapshot to the final impl.
FINAL = $(IMPL)/event-loop

all: brrr brrr-threads

brrr: $(FINAL)/brrr.c
	$(CC) $(CFLAGS) -o $@ $< -lsqlite3

brrr-threads: $(IMPL)/thread-per-connection/brrr_threads.c
	$(CC) $(CFLAGS) -pthread -o $@ $< -lsqlite3

# compile-check every historical snapshot
variants:
	@for f in $(IMPL)/event-loop-*/brrr.c; do \
	  $(CC) $(CFLAGS) -o /dev/null $$f -lsqlite3 || exit 1; \
	done; echo "all variants compile"

bench: brrr
	python3 bench/bench.py 8080 20000 200

capacity: brrr
	python3 bench/capacity.py 127.0.0.1 8080 20000 4000

proof: Brrr.lean
	~/.elan/bin/lean Brrr.lean

clean:
	rm -f brrr brrr-threads

.PHONY: all variants bench capacity proof clean
