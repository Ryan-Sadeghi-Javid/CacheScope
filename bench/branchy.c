/* Branchy versus branchless: the same computation expressed two ways.
 *
 * Both loops compute the sum of values >= 128 over the same data. The branchy
 * version uses a data-dependent conditional; the branchless version uses a mask
 * so the result is identical with no control-flow dependence at all.
 *
 * Expected behaviour: on shuffled data the branchy loop mispredicts about half
 * its branches and runs several times slower, while the branchless loop is
 * unaffected. On sorted data the branch becomes perfectly predictable and the
 * gap disappears. That contrast is what the predictor comparison should show.
 *
 * This benchmark also writes a branch trace of its data-dependent branch when
 * given an output path, so the simulated predictor sees exactly the branch the
 * hardware saw.
 *
 * Usage: branchy [count] [sorted] [branch_trace_path]
 *        sorted: 0 = shuffled (unpredictable), 1 = sorted (predictable)
 */
#include "bench_util.h"

int main(int argc, char** argv) {
  const long count = bench_arg(argc, argv, 1, 4 * 1000 * 1000);
  const long sorted = bench_arg(argc, argv, 2, 0) == 1 ? 1 : 0;
  const char* trace_path = (argc > 3) ? argv[3] : NULL;

  uint8_t* data = (uint8_t*)malloc((size_t)count);
  if (!data) {
    fprintf(stderr, "branchy: allocation of %ld bytes failed\n", count);
    return 1;
  }
  bench_srand(0xB4A9C4);
  for (long i = 0; i < count; ++i) data[i] = (uint8_t)(bench_rand() & 0xFF);

  if (sorted) {
    /* Counting sort: the data is bytes, so this is exact and linear. */
    size_t hist[256];
    memset(hist, 0, sizeof(hist));
    for (long i = 0; i < count; ++i) hist[data[i]]++;
    long w = 0;
    for (int v = 0; v < 256; ++v) {
      for (size_t k = 0; k < hist[v]; ++k) data[w++] = (uint8_t)v;
    }
  }

  /* Branchy. */
  uint64_t sum_branchy = 0;
  const double b0 = bench_now();
  for (long i = 0; i < count; ++i) {
    if (data[i] >= 128) {
      sum_branchy += data[i];
    }
  }
  const double b1 = bench_now();

  /* Branchless: the same arithmetic via a mask derived from the comparison. */
  uint64_t sum_branchless = 0;
  const double c0 = bench_now();
  for (long i = 0; i < count; ++i) {
    const uint8_t v = data[i];
    /* (v >= 128) is 0 or 1; negating gives an all-ones or all-zeros mask. */
    const uint64_t mask = (uint64_t)0 - (uint64_t)(v >> 7);
    sum_branchless += (uint64_t)v & mask;
  }
  const double c1 = bench_now();

  if (sum_branchy != sum_branchless) {
    fprintf(stderr, "branchy: INTERNAL ERROR, the two loops disagree (%llu vs %llu)\n",
            (unsigned long long)sum_branchy, (unsigned long long)sum_branchless);
    free(data);
    return 2;
  }

  bench_report(sorted ? "branchy_sorted" : "branchy_shuffled", (uint64_t)count, (uint64_t)sorted,
               b1 - b0, (uint64_t)count, sum_branchy);
  bench_report(sorted ? "branchless_sorted" : "branchless_shuffled", (uint64_t)count,
               (uint64_t)sorted, c1 - c0, (uint64_t)count, sum_branchless);

  /* Emit the exact branch outcomes the hardware just executed, so the
   * simulator can be fed the real sequence rather than a synthetic one. */
  if (trace_path) {
    FILE* f = fopen(trace_path, "wb");
    if (!f) {
      fprintf(stderr, "branchy: cannot write branch trace to %s\n", trace_path);
      free(data);
      return 1;
    }
    fprintf(f, "# branch trace from bench/branchy.c (%s data)\n",
            sorted ? "sorted" : "shuffled");
    fprintf(f, "# format: <hex pc> <t|n>; pc 0x400100 is the data-dependent branch\n");
    for (long i = 0; i < count; ++i) {
      fputs(data[i] >= 128 ? "0x400100 t\n" : "0x400100 n\n", f);
    }
    fclose(f);
    fprintf(stderr, "branchy: wrote %ld branch records to %s\n", count, trace_path);
  }

  free(data);
  return 0;
}
