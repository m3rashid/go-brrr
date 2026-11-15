#!/bin/sh
# entry.sh — run brrr with the allocation counter (bench/alloc_hook.c) preloaded.
export LD_PRELOAD=/usr/local/lib/allochook.so
export ALLOC_STATS=/tmp/alloc.stats
exec /usr/local/bin/brrr "$@"
