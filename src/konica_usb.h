/*
 * Direct libusb transport for the Konica Minolta bizhub 206.
 *
 * Replaces /usr/lib/cups/backend/usb and the custom konica-backend/usb sender.
 * Key rule from the field: never write more than 8192 bytes per bulk transfer,
 * larger writes make the printer drop off the bus.
 */

#ifndef KONICA_USB_H
#define KONICA_USB_H

#include <stddef.h>
#include <sys/types.h>
#include <libusb-1.0/libusb.h>

#define KONICA_VID          0x132b
#define KONICA_PID          0x232b
#define KONICA_IFACE        1
#define KONICA_CHUNK        8192
#define KONICA_TIMEOUT_MS   10000   /* per bulk transfer */
#define KONICA_MAX_STALLS   6       /* consecutive zero-progress timeouts before giving up */

#define KONICA_DEVICE_ID    "MFG:KONICA MINOLTA;CMD:GDI,XPS;MDL:206;PRINTER"

typedef struct konica_usb_s
{
  libusb_context       *ctx;
  libusb_device_handle *h;
  int                  iface;
  unsigned char        ep_out;
  unsigned char        ep_in;      /* 0 if none */
  char                 serial[64];
  char                 err[160];   /* last error, human readable */
} konica_usb_t;

/* Open the first matching printer. serial may be NULL/"" to match any.
 * Returns NULL on failure; if err is non-NULL it receives the reason.
 * errno-style codes: ENODEV (not found), EBUSY (claim failed), EIO (other). */
konica_usb_t *konica_usb_open(const char *serial, char *err, size_t errsize);

/* Write the whole buffer in <= KONICA_CHUNK transfers.
 * Returns bytes written (== len) or -1 with errno set (ENODEV if unplugged,
 * ETIMEDOUT if the printer stopped accepting data). */
ssize_t konica_usb_write(konica_usb_t *u, const void *buf, size_t len);

/* Best-effort bulk IN read with a short timeout. Returns bytes, 0 if none, -1 on error. */
ssize_t konica_usb_read(konica_usb_t *u, void *buf, size_t len, unsigned timeout_ms);

void konica_usb_close(konica_usb_t *u);

/* Enumerate attached printers: calls cb(serial, userdata) for each. Returns count. */
int konica_usb_list(void (*cb)(const char *serial, void *ud), void *ud);

#endif
