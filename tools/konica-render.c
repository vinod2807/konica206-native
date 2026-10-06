/* Stage 1 test tool: render a PDF to printer data on disk. Never touches USB.
 *   konica-render [-o options] in.pdf out.bin
 * Example options: "PageSize=A4 Duplex=DuplexNoTumble Resolution=600dpi" */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "konica_filter.h"

int
main(int argc, char **argv)
{
  konica_cfg_t cfg;
  char opts[512] = "", log[8192] = "";
  int opt;
  struct stat st;

  konica_cfg_load(&cfg);
  while ((opt = getopt(argc, argv, "o:")) != -1)
    if (opt == 'o')
      snprintf(opts, sizeof(opts), "%s", optarg);
  if (argc - optind != 2)
  {
    fprintf(stderr, "usage: %s [-o \"cups options\"] in.pdf out.bin\n", argv[0]);
    return 2;
  }
  if (!opts[0])
    konica_build_options(&cfg, opts, sizeof(opts), "iso_a4_210x297mm", "one-sided", 600, 600);

  fprintf(stderr, "raster filter : %s\nvendor filter : %s\nPPD           : %s\nlibdir        : %s\noptions       : %s\n",
          cfg.raster_filter, cfg.vendor_filter, cfg.ppd, cfg.libdir, opts);

  if (konica_render_pdf(&cfg, argv[optind], argv[optind + 1], 1, getenv("USER") ? getenv("USER") : "user",
                        "test", opts, NULL, NULL, log, sizeof(log)) != 0)
  {
    fprintf(stderr, "RENDER FAILED\n%s", log);
    return 1;
  }
  if (log[0])
    fprintf(stderr, "filter stderr:\n%s", log);
  stat(argv[optind + 1], &st);
  fprintf(stderr, "OK: wrote %lld bytes to %s\n", (long long)st.st_size, argv[optind + 1]);
  return 0;
}
