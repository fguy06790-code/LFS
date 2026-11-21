# lfs-setup: system configuration menu for Linux From Scratch
PREFIX  ?= /usr
CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -Wall -Wextra
LDLIBS  := -lncursesw

all: lfs-setup

lfs-setup: lfs-setup.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

install: lfs-setup
	install -Dm755 lfs-setup $(DESTDIR)$(PREFIX)/sbin/lfs-setup
	install -Dm644 lfs-setup.desktop $(DESTDIR)$(PREFIX)/share/applications/lfs-setup.desktop
	install -Dm644 lfs-setup-hint.sh $(DESTDIR)/etc/profile.d/lfs-setup-hint.sh
	install -Dm644 lfs-setup.8 $(DESTDIR)$(PREFIX)/share/man/man8/lfs-setup.8

clean:
	rm -f lfs-setup

.PHONY: all install clean
