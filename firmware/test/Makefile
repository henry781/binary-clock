# PC test bench for the DCF77 decoder (see README.md in this directory).
#
#   make          build the simulator
#   make test     quick regression suite (about 10 s, run by CI)
#   make bench    full statistics (several minutes)

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  := -lm

dcf77_sim: dcf77_sim.c ../binary_clock/dcf77_decoder.h
	$(CC) $(CFLAGS) -o $@ dcf77_sim.c $(LDLIBS)

test: dcf77_sim
	./run_tests.sh

bench: dcf77_sim
	./bench.sh

clean:
	rm -f dcf77_sim

.PHONY: test bench clean
