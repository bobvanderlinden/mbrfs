.PHONY: force

CFLAGS = $(shell pkg-config fuse --cflags)
LDFLAGS = $(shell pkg-config fuse --libs)

all: mbrfs

mbrfs: mbrfs.c
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@

test: force
	./mbrfs test.img -f test
