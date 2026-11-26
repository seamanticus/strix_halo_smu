CFLAGS ?= -O2
CFLAGS += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -I.. -fPIC

all: libshs.so shsctl

libshs.so: libshs.c shs.h ../strix_halo_smu.h
	$(CC) $(CFLAGS) -shared -o $@ libshs.c -lm

shsctl: shsctl.c libshs.c shs.h ../strix_halo_smu.h
	$(CC) $(CFLAGS) -o $@ shsctl.c libshs.c -lm

clean:
	rm -f libshs.so shsctl

.PHONY: all clean
