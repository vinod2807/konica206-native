/*
 * konica206-native: standalone PAPPL Printer Application for the
 * Konica Minolta bizhub 206 (USB, GDI-only).
 *
 *   IPP client -> PAPPL -> pdftoraster -> 245igdirf -> libusb (8192-byte chunks)
 *
 * No cupsd PPD queue, CUPS filter or CUPS USB backend is involved at runtime.
 */

#include <pappl/pappl.h>
#include <cups/raster.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "konica_filter.h"
#include "konica_usb.h"
#include "konica_watch.h"

#if CUPS_VERSION_MAJOR >= 3
#  define CUPS_LEN_T cups_len_t
#else
#  define CUPS_LEN_T int
#endif

#define VERSION "0.1.0"
#define DRIVER_NAME "konica-bizhub-206"

static konica_cfg_t cfg;

/* ---------------------------------------------------------------------- */
/* Device scheme "konica://": libusb transport with 8192-byte chunking     */
/* ---------------------------------------------------------------------- */

typedef struct { pappl_device_cb_t cb; void *data; } list_ctx_t;

static void
list_one(const char *serial, void *ud)
{
  list_ctx_t *lc = ud;
  char uri[256], info[128];

  if (cfg.serial[0] && serial[0] && strcmp(cfg.serial, serial))
    return;
  snprintf(uri, sizeof(uri), "konica://KONICA%%20MINOLTA/206?serial=%s", serial);
  snprintf(info, sizeof(info), "KONICA MINOLTA 206 (%s)", serial[0] ? serial : "no serial");
  (lc->cb)(info, uri, KONICA_DEVICE_ID, lc->data);
}

static bool
dev_list(pappl_device_cb_t cb, void *data, pappl_deverror_cb_t err_cb, void *err_data)
{
  list_ctx_t lc = { cb, data };

  (void)err_cb;
  (void)err_data;
  konica_usb_list(list_one, &lc);
  return false;   /* false = keep going with other schemes */
}

static bool
dev_open(pappl_device_t *device, const char *device_uri, const char *name)
{
  const char *q = strstr(device_uri, "serial=");
  char serial[64] = "", err[200] = "";
  konica_usb_t *u;

  (void)name;
  if (q)
    snprintf(serial, sizeof(serial), "%s", q + 7);
  else
    snprintf(serial, sizeof(serial), "%s", cfg.serial);

  if (!(u = konica_usb_open(serial, err, sizeof(err))))
  {
    papplDeviceError(device, "%s", err);
    return false;
  }
  papplDeviceSetData(device, u);
  return true;
}

static void
dev_close(pappl_device_t *device)
{
  konica_usb_close(papplDeviceGetData(device));
  papplDeviceSetData(device, NULL);
}

static ssize_t
dev_read(pappl_device_t *device, void *buffer, size_t bytes)
{
  return konica_usb_read(papplDeviceGetData(device), buffer, bytes, 500);
}

static ssize_t
dev_write(pappl_device_t *device, const void *buffer, size_t bytes)
{
  konica_usb_t *u = papplDeviceGetData(device);
  ssize_t r = konica_usb_write(u, buffer, bytes);

  if (r < 0)
    papplDeviceError(device, "USB write failed: %s", u->err[0] ? u->err : strerror(errno));
  return r;
}

static pappl_preason_t
dev_status(pappl_device_t *device)
{
  (void)device;
  return PAPPL_PREASON_NONE;
}

static char *
dev_id(pappl_device_t *device, char *buffer, size_t bufsize)
{
  (void)device;
  snprintf(buffer, bufsize, "%s", KONICA_DEVICE_ID);
  return buffer;
}

/* ---------------------------------------------------------------------- */
/* Test-only scheme "konicafile:///dir": job output lands in dir/job-N.bin    */
/* so the whole IPP -> PAPPL -> filters path can be exercised without USB.   */
/* ---------------------------------------------------------------------- */

static bool
file_open(pappl_device_t *device, const char *device_uri, const char *name)
{
  static int seq;
  char path[1024];
  FILE *fp;

  (void)name;
  snprintf(path, sizeof(path), "%s/job-%d-%d.bin", device_uri + strlen("konicafile://"), (int)getpid(), ++seq);
  if (!(fp = fopen(path, "wb")))
  {
    papplDeviceError(device, "Unable to create %s: %s", path, strerror(errno));
    return false;
  }
  papplDeviceSetData(device, fp);
  return true;
}

static void file_close(pappl_device_t *device) { if (papplDeviceGetData(device)) fclose(papplDeviceGetData(device)); }
static ssize_t file_read(pappl_device_t *device, void *b, size_t n) { (void)device; (void)b; (void)n; return 0; }
static ssize_t file_write(pappl_device_t *device, const void *b, size_t n) { return (ssize_t)fwrite(b, 1, n, papplDeviceGetData(device)); }
static pappl_preason_t file_status(pappl_device_t *device) { (void)device; return PAPPL_PREASON_NONE; }
static char *file_id(pappl_device_t *device, char *b, size_t n) { (void)device; snprintf(b, n, "%s", KONICA_DEVICE_ID); return b; }

/* ---------------------------------------------------------------------- */
/* Job handling                                                            */
/* ---------------------------------------------------------------------- */

static int
job_cancel_cb(void *ud)
{
  /* Called repeatedly by long renders: each call proves the job is alive. */
  konica_watch_ping((pappl_job_t *)ud);
  return papplJobIsCanceled((pappl_job_t *)ud);
}

static const char *
sides_string(pappl_sides_t s)
{
  switch (s)
  {
    case PAPPL_SIDES_TWO_SIDED_LONG_EDGE:  return "two-sided-long-edge";
    case PAPPL_SIDES_TWO_SIDED_SHORT_EDGE: return "two-sided-short-edge";
    default:                               return "one-sided";
  }
}

/* Send a finished printer stream (all copies) over the device. Success is only
 * reported after every byte of every copy was accepted by the printer. */
static bool
send_spool(pappl_job_t *job, pappl_device_t *device, const char *spool, int copies)
{
  FILE *fp = fopen(spool, "rb");
  bool ok = false;

  if (!fp)
    return false;

  /* Copies: replay the complete, self-contained printer stream (collated). */
  for (int c = 0; c < copies; c++)
  {
    unsigned char buf[KONICA_CHUNK];
    size_t n;

    rewind(fp);
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
    {
      if (papplJobIsCanceled(job))
        goto done;
      konica_watch_ping(job);
      if (papplDeviceWrite(device, buf, n) < 0)
      {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "USB transfer failed (copy %d)", c + 1);
        papplJobSetReasons(job, PAPPL_JREASON_ABORTED_BY_SYSTEM, PAPPL_JREASON_NONE);
        goto done;
      }
    }
    papplDeviceFlush(device);
  }
  papplJobSetImpressions(job, 1);
  ok = true;

done:
  fclose(fp);
  return ok;
}

static bool
konica_printfile(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device)
{
  const char *pdf = papplJobGetFilename(job);
  char spool[700], opts[512], log[4096] = "";
  int fd, copies = options->copies > 0 ? options->copies : 1;
  bool ok = false;

  snprintf(spool, sizeof(spool), "%s/konica-out-XXXXXX", cfg.spooldir);
  if ((fd = mkstemp(spool)) < 0)
  {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unable to create spool file: %s", strerror(errno));
    papplJobSetReasons(job, PAPPL_JREASON_ABORTED_BY_SYSTEM, PAPPL_JREASON_NONE);
    return false;
  }
  close(fd);

  konica_build_options(&cfg, opts, sizeof(opts), options->media.size_name,
                       sides_string(options->sides),
                       options->printer_resolution[0], options->printer_resolution[1]);
  {
    char cinfo[32];
    snprintf(cinfo, sizeof(cinfo), "%d", copies);     /* PAPPL's own %d logging prints blanks on some builds */
    papplLogJob(job, PAPPL_LOGLEVEL_INFO, "Rendering %s (copies=%s) with options: %s", pdf, cinfo, opts);
  }

  /* Render fully BEFORE touching the printer: a failed render never sends a partial job. */
  konica_watch_job_start(job);
  if (konica_render_pdf(&cfg, pdf, spool, papplJobGetID(job), papplJobGetUsername(job),
                        papplJobGetName(job), opts, job_cancel_cb, job, log, sizeof(log)) != 0)
  {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Render failed: %s", log);
    papplJobSetReasons(job, PAPPL_JREASON_ABORTED_BY_SYSTEM, PAPPL_JREASON_NONE);
    goto out;
  }
  if (log[0])
    papplLogJob(job, PAPPL_LOGLEVEL_DEBUG, "Filter output: %s", log);

  ok = send_spool(job, device, spool, copies);

out:
  konica_watch_job_done(job);
  unlink(spool);
  return ok;
}

/* ---- raster path (image/pwg-raster, image/urf, image/jpeg, image/png) ------
 * PAPPL rasterizes to 8-bit gray lines; we wrap them as a CUPS raster stream and
 * hand that to the vendor filter, exactly like pdftoraster would.
 * NOTE: PAPPL 1.3 requires all five raster callbacks even for a PDF-only driver. */

typedef struct
{
  char          path[700];      /* CUPS raster temp file */
  int           fd;
  cups_raster_t *ras;
  bool          failed;
} rjob_t;

static bool
r_startjob(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device)
{
  rjob_t *r = calloc(1, sizeof(*r));

  (void)options; (void)device;
  if (!r)
    return false;
  snprintf(r->path, sizeof(r->path), "%s/konica-ras-XXXXXX", cfg.spooldir);
  if ((r->fd = mkstemp(r->path)) < 0 || !(r->ras = cupsRasterOpen(r->fd, CUPS_RASTER_WRITE_COMPRESSED)))
  {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unable to open raster spool: %s", strerror(errno));
    if (r->fd >= 0) { close(r->fd); unlink(r->path); }
    free(r);
    return false;
  }
  papplJobSetData(job, r);
  konica_watch_job_start(job);
  return true;
}

static bool
r_startpage(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device, unsigned page)
{
  rjob_t *r = papplJobGetData(job);

  (void)device; (void)page;
  /* Duplex/Tumble from IPP "sides" (see plan: one-sided / long-edge / short-edge). */
  options->header.Duplex = options->sides != PAPPL_SIDES_ONE_SIDED;
  options->header.Tumble = options->sides == PAPPL_SIDES_TWO_SIDED_SHORT_EDGE;
#if CUPS_VERSION_MAJOR >= 3
  if (!cupsRasterWriteHeader(r->ras, &options->header))
#else
  if (!cupsRasterWriteHeader2(r->ras, &options->header))
#endif
  {
    r->failed = true;
    return false;
  }
  return true;
}

static bool
r_writeline(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device, unsigned y, const unsigned char *line)
{
  rjob_t *r = papplJobGetData(job);

  (void)device; (void)y;
  if (cupsRasterWritePixels(r->ras, (unsigned char *)line, options->header.cupsBytesPerLine) != options->header.cupsBytesPerLine)
  {
    r->failed = true;
    return false;
  }
  /* Raster lines arrive thousands per page; ping cheaply every 64 lines. */
  {
    static __thread unsigned ping_n = 0;
    if (++ping_n % 64 == 0)
      konica_watch_ping(job);
  }
  return true;
}

static bool
r_endpage(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device, unsigned page)
{
  (void)job; (void)options; (void)device; (void)page;
  return true;
}

static bool
r_endjob(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device)
{
  rjob_t *r = papplJobGetData(job);
  char spool[700], opts[512], log[4096] = "";
  int fd;
  bool ok = false;

  if (!r)
    return false;
  cupsRasterClose(r->ras);
  close(r->fd);
  papplJobSetData(job, NULL);

  snprintf(spool, sizeof(spool), "%s/konica-out-XXXXXX", cfg.spooldir);
  if ((fd = mkstemp(spool)) < 0)
    goto out;
  close(fd);

  konica_build_options(&cfg, opts, sizeof(opts), options->media.size_name, sides_string(options->sides),
                       options->printer_resolution[0], options->printer_resolution[1]);

  if (r->failed || papplJobIsCanceled(job))
    goto out2;
  if (konica_render_raster(&cfg, r->path, spool, papplJobGetID(job), papplJobGetUsername(job),
                           papplJobGetName(job), opts, job_cancel_cb, job, log, sizeof(log)) != 0)
  {
    papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Render failed: %s", log);
    papplJobSetReasons(job, PAPPL_JREASON_ABORTED_BY_SYSTEM, PAPPL_JREASON_NONE);
    goto out2;
  }
  ok = send_spool(job, device, spool, options->copies > 0 ? options->copies : 1);

out2:
  unlink(spool);
out:
  unlink(r->path);
  free(r);
  konica_watch_job_done(job);
  return ok;
}

static bool
konica_status(pappl_printer_t *printer)
{
  (void)printer;
  return true;
}

/* ---------------------------------------------------------------------- */
/* Driver data                                                             */
/* ---------------------------------------------------------------------- */

static const char *media_names[] =
{
  "iso_a3_297x420mm", "iso_a4_210x297mm", "iso_a5_148x210mm", "iso_a6_105x148mm",
  "iso_b4_250x353mm", "iso_b5_176x250mm",
  "na_letter_8.5x11in", "na_legal_8.5x14in", "na_executive_7.25x10.5in", "na_invoice_5.5x8.5in"
};

static bool
konica_driver(pappl_system_t *system, const char *driver_name, const char *device_uri,
              const char *device_id, pappl_pr_driver_data_t *d, ipp_t **attrs, void *data)
{
  (void)system; (void)device_uri; (void)device_id; (void)attrs; (void)data;

  if (strcmp(driver_name, DRIVER_NAME))
    return false;

  d->printfile_cb = konica_printfile;
  d->rstartjob_cb = r_startjob;
  d->rstartpage_cb = r_startpage;
  d->rwriteline_cb = r_writeline;
  d->rendpage_cb  = r_endpage;
  d->rendjob_cb   = r_endjob;
  d->status_cb    = konica_status;
  d->format       = "application/pdf";      /* PDF goes to printfile_cb untouched */

  snprintf(d->make_and_model, sizeof(d->make_and_model), "KONICA MINOLTA 206");
  d->ppm = 20;
  d->kind = PAPPL_KIND_DOCUMENT;

  d->color_supported = PAPPL_COLOR_MODE_MONOCHROME;
  d->color_default   = PAPPL_COLOR_MODE_MONOCHROME;
  d->content_default = PAPPL_CONTENT_AUTO;
  d->quality_default = IPP_QUALITY_NORMAL;
  d->scaling_default = PAPPL_SCALING_AUTO;
  d->orient_default  = IPP_ORIENT_NONE;

  d->raster_types = PAPPL_PWG_RASTER_TYPE_SGRAY_8;

  /* Full-page geometry (matches KonicaMinolta-206-fullbleed.ppd): no margins. */
  d->borderless  = true;
  d->left_right  = 0;
  d->bottom_top  = 0;

  d->num_resolution = 2;
  d->x_resolution[0] = d->y_resolution[0] = 300;
  d->x_resolution[1] = d->y_resolution[1] = 600;
  d->x_default = d->y_default = 600;

  d->duplex          = PAPPL_DUPLEX_NORMAL;
  d->sides_supported = PAPPL_SIDES_ONE_SIDED | PAPPL_SIDES_TWO_SIDED_LONG_EDGE | PAPPL_SIDES_TWO_SIDED_SHORT_EDGE;
  d->sides_default   = PAPPL_SIDES_ONE_SIDED;

  d->num_media = (int)(sizeof(media_names) / sizeof(media_names[0]));
  for (int i = 0; i < d->num_media; i++)
    d->media[i] = media_names[i];

  /* A4 is the default. Do NOT inherit PAPPL's Letter default. */
  memset(&d->media_default, 0, sizeof(d->media_default));
  papplCopyString(d->media_default.size_name, "iso_a4_210x297mm", sizeof(d->media_default.size_name));
  d->media_default.size_width  = 21000;
  d->media_default.size_length = 29700;
  papplCopyString(d->media_default.source, "auto", sizeof(d->media_default.source));
  papplCopyString(d->media_default.type, "stationery", sizeof(d->media_default.type));

  /* TODO(stage 4): confirm tray names against the PPD *InputSlot list. */
  d->num_source = 2;                 /* PAPPL adds "auto" itself */
  d->source[0] = "tray-1";
  d->source[1] = "by-pass-tray";

  d->num_type = 1;
  d->type[0] = "stationery";

  for (int i = 0; i < d->num_source; i++)
    d->media_ready[i] = d->media_default;
  papplCopyString(d->media_ready[0].source, "tray-1", sizeof(d->media_ready[0].source));
  papplCopyString(d->media_ready[1].source, "by-pass-tray", sizeof(d->media_ready[1].source));

  return true;
}

static const char *
konica_autoadd(const char *device_info, const char *device_uri, const char *device_id, void *data)
{
  (void)device_info; (void)data;
  if (!strncmp(device_uri, "konica://", 9) || (device_id && strstr(device_id, "KONICA MINOLTA") && strstr(device_id, "MDL:206")))
    return DRIVER_NAME;
  return NULL;
}

/* ---------------------------------------------------------------------- */

static pappl_system_t *
system_cb(int num_options, cups_option_t *options, void *data)
{
  pappl_system_t *system;
  const char *val;
  int port = 8001;                         /* dev port; leaves konica206uri alone */
  static pappl_pr_driver_t drivers[] =
  {
    { DRIVER_NAME, "KONICA MINOLTA bizhub 206", KONICA_DEVICE_ID, NULL }
  };
  pappl_version_t ver = { "konica206-native", "", VERSION, { 0, 1, 0, 0 } };

  (void)data;
  if ((val = cupsGetOption("server-port", (CUPS_LEN_T)num_options, options)) != NULL)
    port = atoi(val);

  const char *logfile = "-";                /* stderr -> journald when run as a service */
  pappl_loglevel_t loglevel = PAPPL_LOGLEVEL_INFO;

  if ((val = cupsGetOption("log-file", (CUPS_LEN_T)num_options, options)) != NULL)
    logfile = val;
  if ((val = cupsGetOption("log-level", (CUPS_LEN_T)num_options, options)) != NULL)
    loglevel = !strcmp(val, "debug") ? PAPPL_LOGLEVEL_DEBUG : !strcmp(val, "error") ? PAPPL_LOGLEVEL_ERROR : PAPPL_LOGLEVEL_INFO;
  if ((val = cupsGetOption("stuck-watch-secs", (CUPS_LEN_T)num_options, options)) != NULL)
    konica_watch_set_secs(atol(val));

  system = papplSystemCreate(PAPPL_SOPTIONS_MULTI_QUEUE | PAPPL_SOPTIONS_WEB_INTERFACE | PAPPL_SOPTIONS_WEB_LOG,
                             "Konica 206 Native", port, "_print,_universal", NULL, logfile,
                             loglevel, NULL, false);
  if (!system)
    return NULL;

  papplSystemAddListeners(system, NULL);
  papplSystemSetPrinterDrivers(system, 1, drivers, konica_autoadd, NULL, konica_driver, NULL);
  papplSystemSetFooterHTML(system, "konica206-native " VERSION);
  papplSystemSetVersions(system, 1, &ver);
  return system;
}

int
main(int argc, char *argv[])
{
  static pappl_pr_driver_t drivers[] =
  {
    { DRIVER_NAME, "KONICA MINOLTA bizhub 206", KONICA_DEVICE_ID, NULL }
  };

  konica_cfg_load(&cfg);
  konica_watch_init();   /* stuck-job watchdog (KONICA_STUCK_SECS, default 600s) */
  papplDeviceAddScheme("konica", PAPPL_DEVTYPE_CUSTOM_LOCAL, dev_list, dev_open, dev_close,
                       dev_read, dev_write, dev_status, dev_id);

  papplDeviceAddScheme("konicafile", PAPPL_DEVTYPE_CUSTOM_LOCAL, NULL, file_open, file_close,
                       file_read, file_write, file_status, file_id);

  return papplMainloop(argc, argv, VERSION, "konica206-native", 1, drivers, konica_autoadd,
                       konica_driver, NULL, NULL, system_cb, NULL, NULL);
}
