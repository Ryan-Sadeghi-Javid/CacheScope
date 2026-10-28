/* Random pointer chase: the canonical latency-bound workload.
 *
 * A permutation of indices is walked one dependent load at a time, so there is
 * no memory-level parallelism to hide latency and no spatial locality for a
 * prefetcher to exploit. Expected behaviour: near-100% L1 and LLC miss rates
 * once the footprint exceeds the LLC, latency-bound rather than
 * bandwidth-bound, and no benefit from either prefetcher.
 *
 * Usage: pointer_chase [kib] [steps]
 */
#include "bench_util.h"

int main(int argc, char** argv) {
  const long kib = bench_arg(argc, argv, 1, 16 * 1024);
  const long steps = bench_arg(argc, argv, 2, 2 * 1000 * 1000);

  const size_t n = (size_t)kib * 1024 / sizeof(size_t);
  size_t* next = (size_t*)malloc(n * sizeof(size_t));
  if (!next) {
    fprintf(stderr, "pointer_chase: allocation of %ld KiB failed\n", kib);
    return 1;
  }

  /* Build a single cycle covering every slot: a Sattolo shuffle of the
   * identity permutation guarantees one cycle of length n, so the walk never
   * revisits a line until it has touched all of them. */
  for (size_t i = 0; i < n; ++i) next[i] = i;
  bench_srand(0xC0FFEE);
  for (size_t i = n - 1; i > 0; --i) {
    const size_t j = (size_t)(bench_rand() % i); /* strictly less than i */
    const size_t tmp = next[i];
    next[i] = next[j];
    next[j] = tmp;
  }

  size_t idx = 0;
  uint64_t touched = 0;
  const double t0 = bench_now();
  for (long s = 0; s < steps; ++s) {
    idx = next[idx]; /* dependent load: cannot be overlapped */
    touched += idx;
  }
  const double t1 = bench_now();

  bench_report("pointer_chase", (uint64_t)n * sizeof(size_t), (uint64_t)steps, t1 - t0,
               (uint64_t)steps, touched);
  free(next);
  return 0;
}
