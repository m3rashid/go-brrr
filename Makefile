CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11

brrr: brrr.c
	$(CC) $(CFLAGS) -o $@ $< -lsqlite3

bench: brrr
	ab -n 200000 -c 256 http://127.0.0.1:8080/

clean:
	rm -f brrr

.PHONY: bench clean
