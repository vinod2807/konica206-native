/* Offline test: konica_usb.c against a fake libusb. No hardware needed.
 * Verifies the 8192-byte rule, partial-transfer handling, stalls and unplug. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "konica_usb.h"

/* ---- fake libusb ------------------------------------------------------- */
static int   max_seen, transfers, total_bytes;
static int   mode;            /* 0 normal, 1 short writes, 2 always timeout, 3 unplug after 3 transfers */
static unsigned char sink[1 << 20];

static struct libusb_context { int x; } fctx;
static struct libusb_device_handle { int x; } fh;
static struct libusb_device { int x; } fdev;
static struct libusb_device *devlist[2] = { &fdev, NULL };

int libusb_init(libusb_context **c) { *c = &fctx; return 0; }
void libusb_exit(libusb_context *c) { (void)c; }
ssize_t libusb_get_device_list(libusb_context *c, libusb_device ***l) { (void)c; *l = devlist; return 1; }
void libusb_free_device_list(libusb_device **l, int u) { (void)l; (void)u; }
int libusb_get_device_descriptor(libusb_device *d, struct libusb_device_descriptor *dd)
{ (void)d; memset(dd, 0, sizeof(*dd)); dd->idVendor = KONICA_VID; dd->idProduct = KONICA_PID; dd->iSerialNumber = 3; return 0; }
int libusb_open(libusb_device *d, libusb_device_handle **h) { (void)d; *h = &fh; return 0; }
void libusb_close(libusb_device_handle *h) { (void)h; }
int libusb_get_string_descriptor_ascii(libusb_device_handle *h, uint8_t i, unsigned char *b, int n)
{ (void)h; (void)i; snprintf((char *)b, n, "A8A6041029423"); return 13; }

static struct libusb_endpoint_descriptor eps[2] =
{ { .bEndpointAddress = 0x01, .bmAttributes = LIBUSB_TRANSFER_TYPE_BULK }, { .bEndpointAddress = 0x82, .bmAttributes = LIBUSB_TRANSFER_TYPE_BULK } };
static struct libusb_interface_descriptor ifd = { .bInterfaceNumber = 1, .bAlternateSetting = 0, .bNumEndpoints = 2, .endpoint = eps };
static struct libusb_interface itf = { .altsetting = &ifd, .num_altsetting = 1 };
static struct libusb_interface itfs[2];
static struct libusb_config_descriptor cfgd = { .bNumInterfaces = 2, .interface = itfs };
int libusb_get_active_config_descriptor(libusb_device *d, struct libusb_config_descriptor **c)
{ (void)d; itfs[0] = (struct libusb_interface){ 0 }; itfs[1] = itf; *c = &cfgd; return 0; }
void libusb_free_config_descriptor(struct libusb_config_descriptor *c) { (void)c; }
int libusb_set_auto_detach_kernel_driver(libusb_device_handle *h, int e) { (void)h; (void)e; return 0; }
int libusb_claim_interface(libusb_device_handle *h, int i) { (void)h; assert(i == 1); return 0; }
int libusb_release_interface(libusb_device_handle *h, int i) { (void)h; (void)i; return 0; }
int libusb_clear_halt(libusb_device_handle *h, unsigned char e) { (void)h; (void)e; return 0; }
const char *libusb_error_name(int e) { (void)e; return "FAKE"; }

int libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *data, int len, int *got, unsigned t)
{
  (void)h; (void)t;
  assert(ep == 0x01);
  if (len > max_seen) max_seen = len;
  transfers++;
  if (mode == 2) { *got = 0; return LIBUSB_ERROR_TIMEOUT; }
  if (mode == 3 && transfers > 3) { *got = 0; return LIBUSB_ERROR_NO_DEVICE; }
  if (mode == 1) len = len > 1000 ? 1000 : len;     /* printer only takes 1000 at a time */
  memcpy(sink + total_bytes, data, len);
  total_bytes += len;
  *got = len;
  return 0;
}

/* ---- tests ------------------------------------------------------------- */
static void reset(int m) { max_seen = transfers = total_bytes = 0; mode = m; }

int main(void)
{
  char err[200];
  static unsigned char big[300000];
  konica_usb_t *u;

  for (size_t i = 0; i < sizeof(big); i++) big[i] = (unsigned char)(i * 31);

  u = konica_usb_open("A8A6041029423", err, sizeof(err));
  assert(u && u->ep_out == 0x01 && u->ep_in == 0x82);
  assert(!strcmp(u->serial, "A8A6041029423"));
  konica_usb_close(u);

  u = konica_usb_open("WRONGSERIAL", err, sizeof(err));
  assert(!u && errno == ENODEV);
  puts("ok  serial matching + dynamic endpoints on interface 1");

  u = konica_usb_open(NULL, err, sizeof(err));
  reset(0);
  assert(konica_usb_write(u, big, sizeof(big)) == (ssize_t)sizeof(big));
  assert(max_seen <= KONICA_CHUNK && max_seen == KONICA_CHUNK);
  assert(total_bytes == (int)sizeof(big) && !memcmp(sink, big, sizeof(big)));
  printf("ok  300000 bytes in %d transfers, largest %d (<= %d), data intact\n", transfers, max_seen, KONICA_CHUNK);

  reset(1);
  assert(konica_usb_write(u, big, 50000) == 50000);
  assert(!memcmp(sink, big, 50000));
  puts("ok  short/partial transfers are resumed without loss");

  reset(2);
  assert(konica_usb_write(u, big, 1000) == -1 && errno == ETIMEDOUT);
  assert(transfers == KONICA_MAX_STALLS);
  puts("ok  stalled printer gives ETIMEDOUT after bounded retries (no hang)");

  reset(3);
  assert(konica_usb_write(u, big, 100000) == -1 && errno == ENODEV);
  puts("ok  unplug mid-job gives ENODEV immediately");

  konica_usb_close(u);
  puts("ALL PASSED");
  return 0;
}
