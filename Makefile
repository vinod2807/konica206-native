CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE
PAPPL_CFLAGS := $(shell pkg-config --cflags pappl)
PAPPL_LIBS   := $(shell pkg-config --libs pappl)
USB_CFLAGS   := $(shell pkg-config --cflags libusb-1.0)
USB_LIBS     := $(shell pkg-config --libs libusb-1.0)

PREFIX ?= /usr/local

all: konica206-native konica-send konica-render test-chunking

konica206-native: src/main.c src/konica_usb.c src/konica_filter.c src/konica_watch.c
	$(CC) $(CFLAGS) $(PAPPL_CFLAGS) $(USB_CFLAGS) -o $@ $^ $(PAPPL_LIBS) $(USB_LIBS) -pthread

# Stage 2: send a captured printer stream straight to the printer (no CUPS, no PAPPL)
konica-send: tools/konica-send.c src/konica_usb.c
	$(CC) $(CFLAGS) -Isrc $(USB_CFLAGS) -o $@ $^ $(USB_LIBS)

# Stage 1: PDF -> printer stream on disk, never touches USB
konica-render: tools/konica-render.c src/konica_filter.c
	$(CC) $(CFLAGS) -Isrc -o $@ $^

# Offline test of the 8192-byte chunk rule using a fake libusb
test-chunking: test/test_chunking.c src/konica_usb.c
	$(CC) $(CFLAGS) -Isrc -o $@ $^

check: test-chunking
	./test-chunking

install: all
	install -D -m755 konica206-native $(DESTDIR)$(PREFIX)/bin/konica206-native
	install -D -m755 konica-send     $(DESTDIR)$(PREFIX)/bin/konica-send
	install -D -m755 konica-render   $(DESTDIR)$(PREFIX)/bin/konica-render

clean:
	rm -f konica206-native konica-send konica-render test-chunking

.PHONY: all check install clean
