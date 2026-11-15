/* alloc_hook.c — LD_PRELOAD allocation counter for benchmarking brrr.
 *
 * Counts malloc/calloc/realloc/free calls and bytes requested, and a
 * background thread dumps counters to $ALLOC_STATS every 200 ms:
 *   t_s=<monotonic> malloc=<n> calloc=<n> realloc=<n> free=<n> bytes=<n>
 *
 * Build: cc -O2 -std=c11 -shared -fPIC -o allochook.so alloc_hook.c -ldl
 * Run:   LD_PRELOAD=./allochook.so ALLOC_STATS=/tmp/alloc.stats ./brrr ...
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static _Atomic unsigned long long a_malloc, a_calloc, a_realloc, a_free, a_bytes;
static void *(*r_malloc)(size_t);
static void *(*r_calloc)(size_t, size_t);
static void *(*r_realloc)(void *, size_t);
static void (*r_free)(void *);
static _Atomic int inited;

/* early allocations during dlsym() init: served from this static buffer */
static char boot[1 << 16];
static size_t boot_used;

static void init_real(void) {
  r_malloc  = (void *(*)(size_t))dlsym(RTLD_NEXT, "malloc");
  r_calloc  = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
  r_realloc = (void *(*)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
  r_free    = (void (*)(void *))dlsym(RTLD_NEXT, "free");
  __atomic_store_n(&inited, 1, __ATOMIC_RELEASE);
}

void *malloc(size_t n) {
  if (!__atomic_load_n(&inited, __ATOMIC_ACQUIRE)) init_real();
  void *p = r_malloc(n);
  if (p) {
    __atomic_add_fetch(&a_malloc, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&a_bytes, n, __ATOMIC_RELAXED);
  }
  return p;
}

void *calloc(size_t n, size_t sz) {
  if (!__atomic_load_n(&inited, __ATOMIC_ACQUIRE)) {
    size_t total = n * sz, round = (total + 15) & ~(size_t)15;
    if (boot_used + round <= sizeof boot) {
      void *p = boot + boot_used;
      boot_used += round;
      return memset(p, 0, total);
    }
    init_real();
  }
  void *p = r_calloc(n, sz);
  if (p) {
    __atomic_add_fetch(&a_calloc, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&a_bytes, n * sz, __ATOMIC_RELAXED);
  }
  return p;
}

void *realloc(void *q, size_t n) {
  if (!__atomic_load_n(&inited, __ATOMIC_ACQUIRE)) init_real();
  void *p = r_realloc(q, n);
  if (p) {
    __atomic_add_fetch(&a_realloc, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&a_bytes, n, __ATOMIC_RELAXED);
  }
  return p;
}

void free(void *q) {
  if (!q) return;
  if ((char *)q >= boot && (char *)q < boot + sizeof boot) return;
  if (r_free) r_free(q);
  __atomic_add_fetch(&a_free, 1, __ATOMIC_RELAXED);
}

static void *dumper(void *arg) {
  (void)arg;
  const char *path = getenv("ALLOC_STATS");
  if (!path) return 0;
  for (;;) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    FILE *f = fopen(path, "w");
    if (f) {
      fprintf(f, "t_s=%.3f malloc=%llu calloc=%llu realloc=%llu free=%llu bytes=%llu\n",
              (double)ts.tv_sec + ts.tv_nsec / 1e9,
              __atomic_load_n(&a_malloc, __ATOMIC_RELAXED),
              __atomic_load_n(&a_calloc, __ATOMIC_RELAXED),
              __atomic_load_n(&a_realloc, __ATOMIC_RELAXED),
              __atomic_load_n(&a_free, __ATOMIC_RELAXED),
              __atomic_load_n(&a_bytes, __ATOMIC_RELAXED));
      fclose(f);
    }
    usleep(200 * 1000);
  }
  return 0;
}

__attribute__((constructor)) static void start(void) {
  init_real();
  pthread_t t;
  if (pthread_create(&t, 0, dumper, 0) == 0) pthread_detach(t);
}
