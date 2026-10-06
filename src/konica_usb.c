#include "konica_usb.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
set_err(konica_usb_t *u, const char *what, int rc)
{
  if (u)
    snprintf(u->err, sizeof(u->err), "%s: %s", what, libusb_error_name(rc));
}

static int
read_serial(libusb_device_handle *h, libusb_device *dev, char *out, size_t outsize)
{
  struct libusb_device_descriptor dd;

  out[0] = '\0';
  if (libusb_get_device_descriptor(dev, &dd) != 0 || !dd.iSerialNumber)
    return -1;
  if (libusb_get_string_descriptor_ascii(h, dd.iSerialNumber, (unsigned char *)out, (int)outsize - 1) < 0)
  {
    out[0] = '\0';
    return -1;
  }
  return 0;
}

/* Find bulk endpoints on KONICA_IFACE alt 0. */
static int
find_endpoints(libusb_device *dev, int iface, unsigned char *ep_out, unsigned char *ep_in)
{
  struct libusb_config_descriptor *cfg;
  int found = 0;

  *ep_out = *ep_in = 0;
  if (libusb_get_active_config_descriptor(dev, &cfg) != 0)
    return -1;

  for (int i = 0; i < cfg->bNumInterfaces; i++)
  {
    const struct libusb_interface *itf = &cfg->interface[i];
    for (int a = 0; a < itf->num_altsetting; a++)
    {
      const struct libusb_interface_descriptor *id = &itf->altsetting[a];
      if (id->bInterfaceNumber != iface || id->bAlternateSetting != 0)
        continue;
      for (int e = 0; e < id->bNumEndpoints; e++)
      {
        const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
        if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
          continue;
        if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT)
        {
          if (!*ep_out) *ep_out = ep->bEndpointAddress;
        }
        else if (!*ep_in)
          *ep_in = ep->bEndpointAddress;
      }
      found = 1;
    }
  }
  libusb_free_config_descriptor(cfg);
  return (found && *ep_out) ? 0 : -1;
}

konica_usb_t *
konica_usb_open(const char *serial, char *err, size_t errsize)
{
  konica_usb_t *u = calloc(1, sizeof(*u));
  libusb_device **list = NULL;
  ssize_t n;
  int rc, saw_match = 0, claim_failed = 0;

  if (!u)
    return NULL;
  u->iface = KONICA_IFACE;

  if ((rc = libusb_init(&u->ctx)) != 0)
  {
    set_err(u, "libusb_init", rc);
    goto fail_errno_io;
  }

  n = libusb_get_device_list(u->ctx, &list);
  for (ssize_t i = 0; i < n; i++)
  {
    struct libusb_device_descriptor dd;
    libusb_device_handle *h;
    char ser[64];

    if (libusb_get_device_descriptor(list[i], &dd) != 0 || dd.idVendor != KONICA_VID || dd.idProduct != KONICA_PID)
      continue;
    if (libusb_open(list[i], &h) != 0)
    {
      saw_match = 1;
      snprintf(u->err, sizeof(u->err), "found %04x:%04x but cannot open it (permissions? try udev rule)", KONICA_VID, KONICA_PID);
      claim_failed = 1;
      continue;
    }
    read_serial(h, list[i], ser, sizeof(ser));
    if (serial && *serial && strcmp(serial, ser))
    {
      libusb_close(h);
      continue;
    }
    saw_match = 1;

    if (find_endpoints(list[i], u->iface, &u->ep_out, &u->ep_in) != 0)
    {
      snprintf(u->err, sizeof(u->err), "no bulk OUT endpoint on interface %d", u->iface);
      libusb_close(h);
      continue;
    }

    libusb_set_auto_detach_kernel_driver(h, 1);
    if ((rc = libusb_claim_interface(h, u->iface)) != 0)
    {
      set_err(u, "claim_interface (another process owns the printer?)", rc);
      claim_failed = 1;
      libusb_close(h);
      continue;
    }

    u->h = h;
    snprintf(u->serial, sizeof(u->serial), "%s", ser);
    libusb_free_device_list(list, 1);
    return u;
  }
  if (list)
    libusb_free_device_list(list, 1);

  if (!saw_match)
    snprintf(u->err, sizeof(u->err), "printer %04x:%04x%s%s not found", KONICA_VID, KONICA_PID,
             (serial && *serial) ? " serial " : "", (serial && *serial) ? serial : "");
  if (err)
    snprintf(err, errsize, "%s", u->err);
  errno = claim_failed ? EBUSY : ENODEV;
  goto fail;

fail_errno_io:
  if (err)
    snprintf(err, errsize, "%s", u->err);
  errno = EIO;
fail:
  {
    int saved = errno;
    if (u->ctx)
      libusb_exit(u->ctx);
    free(u);
    errno = saved;
  }
  return NULL;
}

ssize_t
konica_usb_write(konica_usb_t *u, const void *buf, size_t len)
{
  const unsigned char *p = buf;
  size_t done = 0;
  int stalls = 0;

  while (done < len)
  {
    size_t want = len - done;
    int got = 0, rc;

    if (want > KONICA_CHUNK)
      want = KONICA_CHUNK;

    rc = libusb_bulk_transfer(u->h, u->ep_out, (unsigned char *)p + done, (int)want, &got, KONICA_TIMEOUT_MS);
    if (got > 0)
    {
      done += (size_t)got;
      stalls = 0;
    }

    if (rc == 0)
      continue;

    if (rc == LIBUSB_ERROR_TIMEOUT)
    {
      /* Printer busy (e.g. warming up / paper handling). Tolerate a few stalls. */
      if (got == 0 && ++stalls >= KONICA_MAX_STALLS)
      {
        snprintf(u->err, sizeof(u->err), "printer stopped accepting data after %zu of %zu bytes", done, len);
        errno = ETIMEDOUT;
        return -1;
      }
      continue;
    }

    if (rc == LIBUSB_ERROR_PIPE)
    {
      libusb_clear_halt(u->h, u->ep_out);
      if (++stalls >= KONICA_MAX_STALLS)
      {
        set_err(u, "endpoint stalled", rc);
        errno = EIO;
        return -1;
      }
      continue;
    }

    set_err(u, "bulk write", rc);
    errno = (rc == LIBUSB_ERROR_NO_DEVICE || rc == LIBUSB_ERROR_IO) ? ENODEV : EIO;
    return -1;
  }
  return (ssize_t)done;
}

ssize_t
konica_usb_read(konica_usb_t *u, void *buf, size_t len, unsigned timeout_ms)
{
  int got = 0, rc;

  if (!u->ep_in)
    return 0;
  rc = libusb_bulk_transfer(u->h, u->ep_in, buf, (int)len, &got, timeout_ms);
  if (rc == 0 || rc == LIBUSB_ERROR_TIMEOUT)
    return got;
  set_err(u, "bulk read", rc);
  errno = (rc == LIBUSB_ERROR_NO_DEVICE) ? ENODEV : EIO;
  return -1;
}

void
konica_usb_close(konica_usb_t *u)
{
  if (!u)
    return;
  if (u->h)
  {
    libusb_release_interface(u->h, u->iface);
    libusb_close(u->h);
  }
  if (u->ctx)
    libusb_exit(u->ctx);
  free(u);
}

int
konica_usb_list(void (*cb)(const char *serial, void *ud), void *ud)
{
  libusb_context *ctx;
  libusb_device **list;
  ssize_t n;
  int count = 0;

  if (libusb_init(&ctx) != 0)
    return 0;
  n = libusb_get_device_list(ctx, &list);
  for (ssize_t i = 0; i < n; i++)
  {
    struct libusb_device_descriptor dd;
    libusb_device_handle *h;
    char ser[64] = "";

    if (libusb_get_device_descriptor(list[i], &dd) != 0 || dd.idVendor != KONICA_VID || dd.idProduct != KONICA_PID)
      continue;
    if (libusb_open(list[i], &h) == 0)
    {
      read_serial(h, list[i], ser, sizeof(ser));
      libusb_close(h);
    }
    count++;
    if (cb)
      cb(ser, ud);
  }
  if (list)
    libusb_free_device_list(list, 1);
  libusb_exit(ctx);
  return count;
}
