/* Stage 2 test tool: send a captured printer stream directly over USB.
 *   konica-send [-s serial] file
 * Uses the same code as the PAPPL app (8192-byte chunks, bounded timeouts). */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "konica_usb.h"

int
main(int argc, char **argv)
{
  const char *serial = getenv("KONICA_SERIAL"), *path;
  char err[200] = "", buf[KONICA_CHUNK];
  konica_usb_t *u;
  FILE *fp;
  size_t n;
  unsigned long total = 0;
  int opt;

  while ((opt = getopt(argc, argv, "s:")) != -1)
    if (opt == 's')
      serial = optarg;
  if (optind >= argc)
  {
    fprintf(stderr, "usage: %s [-s serial] printer-stream-file\n", argv[0]);
    return 2;
  }
  path = argv[optind];

  if (!(fp = fopen(path, "rb")))
  {
    perror(path);
    return 1;
  }
  if (!(u = konica_usb_open(serial, err, sizeof(err))))
  {
    fprintf(stderr, "open failed: %s\n", err);
    return 1;
  }
  fprintf(stderr, "opened serial=%s out=0x%02x in=0x%02x\n", u->serial, u->ep_out, u->ep_in);

  while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
  {
    if (konica_usb_write(u, buf, n) < 0)
    {
      fprintf(stderr, "write failed after %lu bytes: %s (%s)\n", total, strerror(errno), u->err);
      konica_usb_close(u);
      return 1;
    }
    total += n;
  }
  fprintf(stderr, "sent %lu bytes\n", total);
  konica_usb_close(u);
  return 0;
}
