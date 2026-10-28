/* Shared helpers for the cachescope microbenchmarks.
 *
 * Each benchmark prints one machine-readable line so the comparison scripts can
 * parse it without guessing:
 *
 *   RESULT name=<kernel> bytes=<footprint> iters=<n> seconds=<t> ns_per_op=<x> checksum=<c>
 *
 * The checksum exists only to stop the optimiser deleting the work; it is
 * printed so that a change in compiler or flags that alters the computation is
 * visible rather than silent.
 */
#ifndef CACHESCOPE_BENCH_UTIL_H
#define CACHESCOPE_BENCH_UTIL_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static inline double bench_now(void) {
#if defined(CLOCK_MONOTONIC)
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#else
  return (double)clock() / (double)CLOCKS_PER_SEC;
#endif
}

static inline void bench_report(const char* name, uint64_t bytes, uint64_t iters, double seconds,
                         uint64_t ops, uint64_t checksum) {
  const double ns_per_op = ops > 0 ? seconds * 1e9 / (double)ops : 0.0;
  printf("RESULT name=%s bytes=%llu iters=%llu seconds=%.6f ns_per_op=%.4f checksum=%llu\n", name,
         (unsigned long long)bytes, (unsigned long long)iters, seconds, ns_per_op,
         (unsigned long long)checksum);
  fflush(stdout);
}

/* xorshift64: a tiny deterministic PRNG so every run and every machine walks
 * the same addresses. Using rand() would make traces non-reproducible. */
static uint64_t bench_rng_state = 0x2545F4914F6CDD1DULL;

static inline void bench_srand(uint64_t seed) {
  bench_rng_state = seed ? seed : 0x2545F4914F6CDD1DULL;
}

static inline uint64_t bench_rand(void) {
  uint64_t x = bench_rng_state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  bench_rng_state = x;
  return x * 0x2545F4914F6CDD1DULL;
}

/* Parses an optional positive long from argv, falling back to a default. */
static inline long bench_arg(int argc, char** argv, int index, long fallback) {
  if (argc <= index) return fallback;
  char* end = NULL;
  const long v = strtol(argv[index], &end, 10);
  if (end == argv[index] || v <= 0) return fallback;
  return v;
}

#endif /* CACHESCOPE_BENCH_UTIL_H */
