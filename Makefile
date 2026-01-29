# mlsblk - list block devices (macOS port of lsblk)
CC = clang
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -framework CoreFoundation

PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin

mlsblk: mlsblk.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: mlsblk
	install -d $(BINDIR)
	install -m 755 mlsblk $(BINDIR)/mlsblk

uninstall:
	rm -f $(BINDIR)/mlsblk

clean:
	rm -f mlsblk

.PHONY: install uninstall clean
