# Microbenchmarks used to validate cachescope against real hardware.
#
# -O2 without -march=native: the point is a build that matches what the
# comparison scripts and CI produce, not the fastest possible binary.
CC      ?= cc
CFLAGS  ?= -O2 -g -std=c11 -Wall -Wextra -fno-omit-frame-pointer
LDLIBS  ?=

BINS = stream pointer_chase matmul branchy

all: $(BINS)

%: %.c bench_util.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(BINS)

.PHONY: all clean
