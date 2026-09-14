# Host build for the pure-C parts of main/: ui_fmt today, layout_pack next.
#
# These have edge cases (prefix-boundary hysteresis, two-unit durations, the
# digits-only-font charset invariant) that are tedious to verify on a panel
# and trivial to verify as a table of assertions.
#
#   make        build and run
#   make asan   the same under ASan + UBSan
CC     ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wshadow -O1 -g -I../main
SRCS    = test_fmt.c ../main/ui_fmt.c

.PHONY: test asan clean
test: h
	./h

h: $(SRCS) ../main/ui_fmt.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

asan: $(SRCS) ../main/ui_fmt.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -o h $(SRCS) -lm
	./h

clean:
	rm -f h
