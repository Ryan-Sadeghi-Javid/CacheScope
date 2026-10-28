/* Determine whether hardware performance counters are actually usable here.
 *
 * `perf stat` failing is often blamed on a missing package or on
 * perf_event_paranoid, but under a hypervisor that does not virtualise the PMU
 * (WSL2 is the common case) the counters are simply absent: perf_event_open
 * returns ENOENT or EOPNOTSUPP no matter how the package is installed or how
 * permissive the sysctl is. This probe asks the kernel directly, so the answer
 * is evidence rather than inference.
 *
 * Build and run:
 *   cc -O2 -o check_perf_support scripts/check_perf_support.c && ./check_perf_support
 *
 * Exit status: 0 if at least one hardware counter opened, 1 otherwise.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/perf_event.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static long perf_event_open_(struct perf_event_attr* attr, pid_t pid, int cpu, int group_fd,
                             unsigned long flags) {
  return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

struct probe {
  const char* name;
  unsigned type;
  unsigned long long config;
};

static const struct probe PROBES[] = {
    {"cycles", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
    {"instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
    {"cache-references", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES},
    {"cache-misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES},
    {"branch-instructions", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS},
    {"branch-misses", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES},
    {"L1-dcache-load-misses", PERF_TYPE_HW_CACHE,
     PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
         (PERF_COUNT_HW_CACHE_RESULT_MISS << 16)},
    {"LLC-load-misses", PERF_TYPE_HW_CACHE,
     PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
         (PERF_COUNT_HW_CACHE_RESULT_MISS << 16)},
    /* A software counter, as a control: this one works even with no PMU, so if
     * it also fails the problem is permissions rather than hardware. */
    {"task-clock (software control)", PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK},
};

int main(void) {
  printf("perf_event_open probe\n");
  printf("=====================\n");

  FILE* f = fopen("/proc/sys/kernel/perf_event_paranoid", "r");
  if (f) {
    int paranoid = -99;
    if (fscanf(f, "%d", &paranoid) == 1) {
      printf("perf_event_paranoid = %d\n", paranoid);
    }
    fclose(f);
  } else {
    printf("perf_event_paranoid = (unreadable)\n");
  }
  printf("\n%-32s %s\n", "counter", "result");
  printf("%-32s %s\n", "--------------------------------", "------------------------");

  int hardware_ok = 0;
  for (size_t i = 0; i < sizeof(PROBES) / sizeof(PROBES[0]); ++i) {
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    attr.type = PROBES[i].type;
    attr.config = PROBES[i].config;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;

    const long fd = perf_event_open_(&attr, 0 /* this process */, -1, -1, 0);
    if (fd < 0) {
      printf("%-32s UNAVAILABLE (%s)\n", PROBES[i].name, strerror(errno));
      continue;
    }

    ioctl((int)fd, PERF_EVENT_IOC_RESET, 0);
    ioctl((int)fd, PERF_EVENT_IOC_ENABLE, 0);
    /* A little work to count. */
    volatile unsigned long long spin = 0;
    for (int k = 0; k < 2000000; ++k) spin += (unsigned long long)k;
    ioctl((int)fd, PERF_EVENT_IOC_DISABLE, 0);

    long long value = -1;
    if (read((int)fd, &value, sizeof(value)) == (ssize_t)sizeof(value)) {
      printf("%-32s ok, counted %lld\n", PROBES[i].name, value);
    } else {
      printf("%-32s opened but unreadable\n", PROBES[i].name);
    }
    close((int)fd);
    if (PROBES[i].type != PERF_TYPE_SOFTWARE) hardware_ok = 1;
  }

  printf("\n");
  if (hardware_ok) {
    printf("VERDICT: hardware counters are available; scripts/run_perf.sh can collect\n");
    printf("         real measurements for the simulated-versus-measured comparison.\n");
    return 0;
  }
  printf("VERDICT: no hardware counters on this machine. `perf stat` will report\n");
  printf("         <not supported> for cycles, instructions and cache events. This is\n");
  printf("         expected under a hypervisor that does not expose the PMU, including\n");
  printf("         WSL2 and most cloud VMs. Run on bare-metal Linux to populate the\n");
  printf("         measured column of docs/perf_comparison.md.\n");
  return 1;
}
