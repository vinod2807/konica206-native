CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE
# Ubuntu ships only pkgconf; override with PKG_CONFIG=pkgconf (or symlink it).
PKG_CONFIG ?= pkg-config
PAPPL_CFLAGS := $(shell $(PKG_CONFIG) --cflags pappl)
PAPPL_LIBS   := $(shell $(PKG_CONFIG) --libs pappl)
USB_CFLAGS   := $(shell $(PKG_CONFIG) --cflags libusb-1.0)
USB_LIBS     := $(shell $(PKG_CONFIG) --libs libusb-1.0)
RASTER_LIBS  := -lcupsimage -lcups

PREFIX ?= /usr/local

all: konica206-native konica-send konica-render test-chunking test-ppd

konica206-native: src/main.c src/konica_usb.c src/konica_filter.c src/konica_watch.c
	$(CC) $(CFLAGS) $(PAPPL_CFLAGS) $(USB_CFLAGS) -o $@ $^ $(PAPPL_LIBS) $(USB_LIBS) -pthread

# Stage 2: send a captured printer stream straight to the printer (no CUPS, no PAPPL)
konica-send: tools/konica-send.c src/konica_usb.c
	$(CC) $(CFLAGS) -Isrc $(USB_CFLAGS) -o $@ $^ $(USB_LIBS)

# Stage 1: PDF -> printer stream on disk, never touches USB
konica-render: tools/konica-render.c src/konica_filter.c
	$(CC) $(CFLAGS) -Isrc -o $@ $^ $(RASTER_LIBS)

# Offline test of the 8192-byte chunk rule using a fake libusb
test-chunking: test/test_chunking.c src/konica_usb.c
	$(CC) $(CFLAGS) -Isrc -o $@ $^

check: test-chunking test-ppd
	./test-chunking
	./test-ppd

# Slow (~40 s): stuck-watchdog policy proof, not part of default check
check-watchdog: test-watchdog
	./test-watchdog

test-watchdog: test/test_watchdog.c src/konica_watch.c
	$(CC) $(CFLAGS) -o $@ $^ $(PAPPL_LIBS) -pthread

install: all
	install -D -m755 konica206-native $(DESTDIR)$(PREFIX)/bin/konica206-native
	install -D -m755 konica-send     $(DESTDIR)$(PREFIX)/bin/konica-send
	install -D -m755 konica-render   $(DESTDIR)$(PREFIX)/bin/konica-render

# PPD parser regression test (no printer, no USB, no paper).
# test_ppd.c #includes src/konica_filter.c, so only the test TU is compiled.
test-ppd: test/test_ppd.c test/ppd-fixture.ppd src/konica_filter.c src/konica_filter.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/test_ppd.c $(RASTER_LIBS)

check-ppd: test-ppd
	./test-ppd

clean:
	rm -f konica206-native konica-send konica-render test-chunking test-ppd

.PHONY: all check install clean
