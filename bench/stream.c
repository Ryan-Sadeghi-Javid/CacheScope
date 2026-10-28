/* Sequential streaming read and write over an array far larger than the LLC.
 *
 * Expected behaviour: every 64-byte line is touched exactly once per pass, so
 * the L1 miss rate should approach (1 / lines_per_access) and the workload
 * should be bandwidth-bound rather than latency-bound. A next-line prefetcher
 * should convert almost all of those misses into hits.
 *
 * Usage: stream [kib] [passes]
 */
#include "bench_util.h"

int main(int argc, char** argv) {
  const long kib = bench_arg(argc, argv, 1, 16 * 1024); /* 16 MiB default */
  const long passes = bench_arg(argc, argv, 2, 4);

  const size_t n = (size_t)kib * 1024 / sizeof(uint64_t);
  uint64_t* a = (uint64_t*)malloc(n * sizeof(uint64_t));
  if (!a) {
    fprintf(stderr, "stream: allocation of %ld KiB failed\n", kib);
    return 1;
  }
  for (size_t i = 0; i < n; ++i) a[i] = i;

  uint64_t sum = 0;
  const double t0 = bench_now();
  for (long p = 0; p < passes; ++p) {
    /* Read-modify-write so the trace contains both loads and stores. */
    for (size_t i = 0; i < n; ++i) {
      sum += a[i];
      a[i] = sum;
    }
  }
  const double t1 = bench_now();

  bench_report("stream", (uint64_t)n * sizeof(uint64_t), (uint64_t)passes, t1 - t0,
               (uint64_t)n * (uint64_t)passes, sum);
  free(a);
  return 0;
}
