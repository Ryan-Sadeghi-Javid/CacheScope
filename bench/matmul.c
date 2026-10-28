/* Naive versus tiled matrix multiply: the textbook demonstration that blocking
 * for cache capacity beats a better asymptotic constant.
 *
 * Naive ijk order strides through B by row length, so each inner iteration
 * touches a different line of B and the reuse distance for B is the whole
 * matrix. Tiling shrinks the working set of the inner loops to three tiles,
 * which fit in L1/L2. Expected behaviour: identical instruction counts and
 * identical results, very different miss rates.
 *
 * Usage: matmul [n] [tile] [mode]   mode: 0 = both, 1 = naive only, 2 = tiled only
 */
#include "bench_util.h"

static void naive(const double* A, const double* B, double* C, long n) {
  for (long i = 0; i < n; ++i) {
    for (long j = 0; j < n; ++j) {
      double acc = 0.0;
      for (long k = 0; k < n; ++k) {
        acc += A[i * n + k] * B[k * n + j];
      }
      C[i * n + j] = acc;
    }
  }
}

static void tiled(const double* A, const double* B, double* C, long n, long t) {
  for (long i = 0; i < n * n; ++i) C[i] = 0.0;
  for (long ii = 0; ii < n; ii += t) {
    for (long jj = 0; jj < n; jj += t) {
      for (long kk = 0; kk < n; kk += t) {
        const long i_max = (ii + t < n) ? ii + t : n;
        const long j_max = (jj + t < n) ? jj + t : n;
        const long k_max = (kk + t < n) ? kk + t : n;
        for (long i = ii; i < i_max; ++i) {
          for (long j = jj; j < j_max; ++j) {
            double acc = C[i * n + j];
            for (long k = kk; k < k_max; ++k) {
              acc += A[i * n + k] * B[k * n + j];
            }
            C[i * n + j] = acc;
          }
        }
      }
    }
  }
}

static uint64_t checksum(const double* C, long n) {
  /* Sum as integers scaled by 1000 so the value is stable across -ffast-math
   * and across the two implementations' different summation orders. */
  double s = 0.0;
  for (long i = 0; i < n * n; ++i) s += C[i];
  return (uint64_t)(s * 1000.0);
}

int main(int argc, char** argv) {
  const long n = bench_arg(argc, argv, 1, 256);
  const long t = bench_arg(argc, argv, 2, 32);
  const long mode = bench_arg(argc, argv, 3, 0);

  double* A = (double*)malloc((size_t)n * n * sizeof(double));
  double* B = (double*)malloc((size_t)n * n * sizeof(double));
  double* C = (double*)malloc((size_t)n * n * sizeof(double));
  if (!A || !B || !C) {
    fprintf(stderr, "matmul: allocation for n=%ld failed\n", n);
    return 1;
  }
  /* Deterministic fill: no PRNG needed, and identical across runs. */
  for (long i = 0; i < n * n; ++i) {
    A[i] = (double)(i % 97) * 0.5;
    B[i] = (double)(i % 89) * 0.25;
  }

  const uint64_t footprint = 3ULL * (uint64_t)n * (uint64_t)n * sizeof(double);
  const uint64_t ops = (uint64_t)n * (uint64_t)n * (uint64_t)n;

  if (mode == 0 || mode == 1) {
    const double t0 = bench_now();
    naive(A, B, C, n);
    const double t1 = bench_now();
    bench_report("matmul_naive", footprint, (uint64_t)n, t1 - t0, ops, checksum(C, n));
  }
  if (mode == 0 || mode == 2) {
    const double t0 = bench_now();
    tiled(A, B, C, n, t);
    const double t1 = bench_now();
    bench_report("matmul_tiled", footprint, (uint64_t)t, t1 - t0, ops, checksum(C, n));
  }

  free(A);
  free(B);
  free(C);
  return 0;
}
