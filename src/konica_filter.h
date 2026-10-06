/*
 * Private filter pipeline: PDF -> CUPS raster -> Konica 245igdirf -> spool file.
 *
 * Filters are run with the standard CUPS filter calling convention
 * (argv: job-id user title copies options [file], environment: PPD, CONTENT_TYPE...)
 * but NOTHING here talks to cupsd. The vendor filter gets a private
 * LD_LIBRARY_PATH so it uses its own bundled libcups/libcupsimage.
 */

#ifndef KONICA_FILTER_H
#define KONICA_FILTER_H

#include <stddef.h>

typedef struct konica_cfg_s
{
  char vendor_filter[512];   /* KONICA_VENDOR_FILTER */
  char raster_filter[512];   /* KONICA_RASTER_FILTER: PDF -> CUPS raster (pdftoraster) */
  char libdir[512];          /* KONICA_LIBDIR: private libcups/libcupsimage */
  char ppd[512];             /* KONICA_PPD: full-bleed PPD the vendor filter reads */
  char serverbin[512];       /* KONICA_SERVERBIN: exported as CUPS_SERVERBIN */
  char datadir[512];         /* KONICA_DATADIR: exported as CUPS_DATADIR */
  char spooldir[512];        /* KONICA_SPOOLDIR: where per-job temp dirs live */
  char serial[64];           /* KONICA_SERIAL: restrict to one printer ("" = any) */
  int  timeout;              /* KONICA_FILTER_TIMEOUT seconds per stage */
} konica_cfg_t;

void konica_cfg_load(konica_cfg_t *cfg);

typedef int (*konica_cancel_cb)(void *ud);   /* return nonzero to abort */

/* Run one filter. in_path (may be NULL) is opened as stdin; out_path receives stdout.
 * argv_file (may be NULL) is passed as the 6th argument.
 * stderr text (truncated) is appended to log. Returns 0 on success, -1 on failure/cancel/timeout. */
int konica_run_filter(const konica_cfg_t *cfg, const char *prog,
                      const char *content_type, const char *final_type,
                      int job_id, const char *user, const char *title,
                      const char *options, const char *argv_file,
                      const char *in_path, const char *out_path,
                      konica_cancel_cb cancel, void *cancel_ud,
                      char *log, size_t logsize);

/* Full render: pdf_path -> out_path (ready-to-send printer data).
 * Creates and removes its own temp dir under cfg->spooldir. */
int konica_render_pdf(const konica_cfg_t *cfg, const char *pdf_path, const char *out_path,
                      int job_id, const char *user, const char *title, const char *options,
                      konica_cancel_cb cancel, void *cancel_ud, char *log, size_t logsize);

/* Vendor stage only: CUPS raster file -> printer data (used for PWG/URF/JPEG/PNG jobs). */
int konica_render_raster(const konica_cfg_t *cfg, const char *raster_path, const char *out_path,
                         int job_id, const char *user, const char *title, const char *options,
                         konica_cancel_cb cancel, void *cancel_ud, char *log, size_t logsize);

/* Build the CUPS option string from IPP-level values.
 * ppd_pagesize may be NULL. Only emits PageSize if the PPD actually defines it. */
void konica_build_options(const konica_cfg_t *cfg, char *out, size_t outsize,
                          const char *pwg_media_name, const char *sides,
                          int xres, int yres);

#endif
