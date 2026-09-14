# Host build of the `prom` component. The whole point of that component having
# no ESP-IDF dependencies is this Makefile: the exposition format has enough
# edge cases (escaped label values, +Inf bucket bounds, counter resets, chunk
# boundaries landing mid-token) that debugging them by flashing a board and
# squinting at a 7" panel would dominate the schedule.
#
#   make        build and run
#   make asan   the same under AddressSanitizer + UBSan  <-- use this one
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wshadow -Wconversion -O1 -g -I../include
SRCS     = main.c ../prom_value.c ../prom_text.c ../prom_ident.c ../prom_math.c

.PHONY: test asan clean
test: t
	./t corpus/*.txt

t: $(SRCS) ../include/*.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

asan: $(SRCS) ../include/*.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -o t $(SRCS) -lm
	./t corpus/*.txt

clean:
	rm -f t
