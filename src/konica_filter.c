#include "konica_filter.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static void konica_child_limits(const konica_cfg_t *cfg);

static void
env_or(char *dst, size_t n, const char *name, const char *def)
{
  const char *v = getenv(name);
  snprintf(dst, n, "%s", (v && *v) ? v : def);
}

void
konica_cfg_load(konica_cfg_t *c)
{
  const char *t;

  memset(c, 0, sizeof(*c));
  env_or(c->vendor_filter, sizeof(c->vendor_filter), "KONICA_VENDOR_FILTER",
         "/usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf");
  env_or(c->raster_filter, sizeof(c->raster_filter), "KONICA_RASTER_FILTER", "/usr/lib/cups/filter/pdftoraster");
  env_or(c->libdir, sizeof(c->libdir), "KONICA_LIBDIR", "/usr/local/lib/konica/lib");
  env_or(c->ppd, sizeof(c->ppd), "KONICA_PPD", "/var/lib/legacy-printer-app/ppd/KonicaMinolta-206-fullbleed.ppd");
  env_or(c->serverbin, sizeof(c->serverbin), "KONICA_SERVERBIN", "/usr/lib/cups");
  env_or(c->datadir, sizeof(c->datadir), "KONICA_DATADIR", "/usr/share/cups");
  env_or(c->spooldir, sizeof(c->spooldir), "KONICA_SPOOLDIR", "/var/tmp");
  env_or(c->serial, sizeof(c->serial), "KONICA_SERIAL", "A8A6041029423");
  t = getenv("KONICA_FILTER_TIMEOUT");
  c->timeout = (t && atoi(t) > 0) ? atoi(t) : 300;
  t = getenv("KONICA_PDF_RENDERER");
  c->pdf_renderer = (t && !strcmp(t, "builtin")) ? 1 : 0;
  t = getenv("KONICA_MAX_MEM_MB");
  c->max_mem_mb = (t && atol(t) >= 0) ? atol(t) : 2048;
  t = getenv("KONICA_MAX_FILE_MB");
  c->max_file_mb = (t && atol(t) >= 0) ? atol(t) : 512;
}

static void
append_log(char *log, size_t logsize, const char *path)
{
  FILE *fp;
  size_t used;

  if (!log || !logsize || !(fp = fopen(path, "r")))
    return;
  used = strlen(log);
  if (used < logsize - 1)
  {
    size_t got = fread(log + used, 1, logsize - 1 - used, fp);
    log[used + got] = '\0';
  }
  fclose(fp);
}

int
konica_run_filter(const konica_cfg_t *cfg, const char *prog,
                  const char *content_type, const char *final_type,
                  int job_id, const char *user, const char *title,
                  const char *options, const char *argv_file,
                  const char *in_path, const char *out_path,
                  konica_cancel_cb cancel, void *cancel_ud,
                  char *log, size_t logsize)
{
  char idbuf[16], errpath[1024 + 8], envbuf[8][1100];
  char *argv[8], *envp[16];
  int ne = 0, na = 0, status = 0, rc = -1;
  pid_t pid;
  time_t start = time(NULL);

  if (access(prog, X_OK) != 0)
  {
    if (log)
      snprintf(log + strlen(log), logsize - strlen(log), "filter not executable: %s (%s)\n", prog, strerror(errno));
    return -1;
  }

  snprintf(idbuf, sizeof(idbuf), "%d", job_id);
  snprintf(errpath, sizeof(errpath), "%s.err", out_path);

  argv[na++] = (char *)prog;
  argv[na++] = idbuf;
  argv[na++] = (char *)(user && *user ? user : "anonymous");
  argv[na++] = (char *)(title && *title ? title : "untitled");
  argv[na++] = "1";                       /* copies are handled by the caller */
  argv[na++] = (char *)(options ? options : "");
  if (argv_file)
    argv[na++] = (char *)argv_file;
  argv[na] = NULL;

  snprintf(envbuf[0], sizeof(envbuf[0]), "PPD=%s", cfg->ppd);
  snprintf(envbuf[1], sizeof(envbuf[1]), "CONTENT_TYPE=%s", content_type);
  snprintf(envbuf[2], sizeof(envbuf[2]), "FINAL_CONTENT_TYPE=%s", final_type);
  snprintf(envbuf[3], sizeof(envbuf[3]), "CUPS_SERVERBIN=%s", cfg->serverbin);
  snprintf(envbuf[4], sizeof(envbuf[4]), "CUPS_DATADIR=%s", cfg->datadir);
  snprintf(envbuf[5], sizeof(envbuf[5]), "LD_LIBRARY_PATH=%s", cfg->libdir);
  snprintf(envbuf[6], sizeof(envbuf[6]), "PRINTER=konica206-native");
  snprintf(envbuf[7], sizeof(envbuf[7]), "TMPDIR=%s", cfg->spooldir);
  for (int i = 0; i < 8; i++)
    envp[ne++] = envbuf[i];
  envp[ne++] = "PATH=/usr/local/bin:/usr/bin:/bin";
  envp[ne++] = "LANG=C";
  envp[ne++] = "CUPS_SERVER=/nonexistent";   /* make sure nothing falls back to cupsd */
  envp[ne] = NULL;

  pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0)
  {
    int in = in_path ? open(in_path, O_RDONLY) : open("/dev/null", O_RDONLY);
    int out = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int err = open(errpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    if (in < 0 || out < 0 || err < 0)
      _exit(126);
    dup2(in, 0);
    dup2(out, 1);
    dup2(err, 2);
    for (int fd = 3; fd < 1024; fd++)
      close(fd);
    setsid();
    konica_child_limits(cfg);
    execve(prog, argv, envp);
    _exit(127);
  }

  for (;;)
  {
    pid_t w = waitpid(pid, &status, WNOHANG);

    if (w == pid)
      break;
    if (w < 0 && errno != EINTR)
      goto done;
    if ((cancel && cancel(cancel_ud)) || time(NULL) - start > cfg->timeout)
    {
      if (log)
        snprintf(log + strlen(log), logsize - strlen(log), "%s: %s\n", prog,
                 (cancel && cancel(cancel_ud)) ? "canceled" : "timed out");
      kill(-pid, SIGKILL);
      waitpid(pid, &status, 0);
      goto done;
    }
    usleep(100000);
  }

  if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    rc = 0;
  else if (log)
  {
    size_t used = strlen(log);
    if (WIFSIGNALED(status))
      snprintf(log + used, logsize - used, "%s: killed by signal %d\n", prog, WTERMSIG(status));
    else
      snprintf(log + used, logsize - used, "%s: exit status %d\n", prog, WEXITSTATUS(status));
  }

done:
  append_log(log, logsize, errpath);
  unlink(errpath);
  return rc;
}

/* Contain hostile/corrupt inputs: no core dumps, bounded CPU/address
 * space/output size. A4 600 dpi gray is ~35 MB/page, so the 2048/512 MiB
 * defaults have wide headroom. Runs in the forked child before exec. */
static void
konica_child_limits(const konica_cfg_t *cfg)
{
  struct rlimit rl;
  rl.rlim_cur = rl.rlim_max = 0;
  setrlimit(RLIMIT_CORE, &rl);
  if (cfg->timeout > 0)
  {
    rl.rlim_cur = rl.rlim_max = (rlim_t)(cfg->timeout + 30);
    setrlimit(RLIMIT_CPU, &rl);
  }
  if (cfg->max_mem_mb > 0)
  {
    rl.rlim_cur = rl.rlim_max = (rlim_t)cfg->max_mem_mb << 20;
    setrlimit(RLIMIT_AS, &rl);
  }
  if (cfg->max_file_mb > 0)
  {
    rl.rlim_cur = rl.rlim_max = (rlim_t)cfg->max_file_mb << 20;
    setrlimit(RLIMIT_FSIZE, &rl);
  }
}

/* Read "*PaperDimension <name>" (points) from the PPD. */
static int
ppd_paper_points(const char *ppd_path, const char *name, double *w, double *h)
{
  FILE *fp = fopen(ppd_path, "r");
  char line[512], key[128];

  if (!fp || !name)
    return -1;
  snprintf(key, sizeof(key), "*PaperDimension %s", name);
  while (fgets(line, sizeof(line), fp))
  {
    if (!strncmp(line, key, strlen(key)))
    {
      double pw = 0, ph = 0;
      if (sscanf(line + strlen(key), ": \"%lf %lf\"", &pw, &ph) == 2 && pw > 0 && ph > 0)
      {
        fclose(fp);
        *w = pw;
        *h = ph;
        return 0;
      }
    }
  }
  fclose(fp);
  return -1;
}

/* MediaPosition number for "*InputSlot <name>" from "<< /MediaPosition N>>". */
static int
ppd_media_position(const char *ppd_path, const char *slot)
{
  FILE *fp = fopen(ppd_path, "r");
  char line[512], key[128];
  int pos = 0;

  if (!fp || !slot || !*slot)
    return 0;
  snprintf(key, sizeof(key), "*InputSlot %s", slot);
  while (fgets(line, sizeof(line), fp))
  {
    if (!strncmp(line, key, strlen(key)))
    {
      const char *mp = strstr(line, "MediaPosition");
      if (mp && sscanf(mp + 13, " %d", &pos) == 1 && pos >= 0)
      {
        fclose(fp);
        return pos;
      }
    }
  }
  fclose(fp);
  return 0;
}

/* Extract "InputSlot=<name>" from a CUPS option string. */
static void
opt_inputslot(const char *options, char *name, size_t n)
{
  const char *p = options ? strstr(options, "InputSlot=") : NULL;

  name[0] = 0;
  if (!p)
    return;
  p += 10;
  size_t i = 0;
  while (*p && *p != ' ' && i + 1 < n)
    name[i++] = *p++;
  name[i] = 0;
}

/* MediaPosition for the requested tray. The options string already carries
 * the PPD slot name (konica_build_options maps IPP tray-1 -> Tray1 etc.),
 * so look "<< /MediaPosition N>>" up directly. Empty/unknown yields 0,
 * exactly like pdftoraster with no InputSlot. */
static int
slot_media_pos(const konica_cfg_t *cfg, const char *options)
{
  char slot[64];

  opt_inputslot(options, slot, sizeof(slot));
  if (!slot[0])
    return 0;
  return ppd_media_position(cfg->ppd, slot);
}

/* Extract "PageSize=<name>" from a CUPS option string. */
static void
opt_pagesize(const char *options, char *name, size_t n)
{
  const char *p = options ? strstr(options, "PageSize=") : NULL;

  name[0] = '\0';
  if (!p)
    return;
  p += 9;
  size_t i = 0;
  while (*p && *p != ' ' && i + 1 < n)
    name[i++] = *p++;
  name[i] = '\0';
}

/* Wait for a child with cancel/timeout handling. Returns 0 on clean exit 0. */
static int
wait_child(pid_t pid, const konica_cfg_t *cfg, konica_cancel_cb cancel, void *cancel_ud,
           const char *what, char *log, size_t logsize)
{
  time_t start = time(NULL);

  for (;;)
  {
    int status = 0;
    pid_t w = waitpid(pid, &status, WNOHANG);

    if (w == pid)
    {
      if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
      if (log)
      {
        size_t used = strlen(log);
        if (WIFSIGNALED(status))
          snprintf(log + used, logsize - used, "%s: killed by signal %d\n", what, WTERMSIG(status));
        else
          snprintf(log + used, logsize - used, "%s: exit status %d\n", what, WEXITSTATUS(status));
      }
      return -1;
    }
    if (w < 0 && errno != EINTR)
      return -1;
    if ((cancel && cancel(cancel_ud)) || time(NULL) - start > cfg->timeout)
    {
      if (log)
        snprintf(log + strlen(log), logsize - strlen(log), "%s: %s\n", what,
                 (cancel && cancel(cancel_ud)) ? "canceled" : "timed out");
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      return -1;
    }
    usleep(100000);
  }
}

/* Ghostscript: PDF -> one 8-bit gray PGM per page at exactly WxH points.
 * Returns page count, or -1 on failure. */
static int
gs_pdf_to_pgm(const konica_cfg_t *cfg, const char *pdf, const char *dir,
              double pw, double ph, int res,
              konica_cancel_cb cancel, void *cancel_ud, char *log, size_t logsize)
{
  char wbuf[128], hbuf[128], dpibuf[64], obuf[768];
  char *argv[16];
  int na = 0, pages = 0;
  pid_t pid;

  snprintf(wbuf, sizeof(wbuf), "-dDEVICEWIDTHPOINTS=%g", pw);
  snprintf(hbuf, sizeof(hbuf), "-dDEVICEHEIGHTPOINTS=%g", ph);
  snprintf(dpibuf, sizeof(dpibuf), "-r%d", res);
  snprintf(obuf, sizeof(obuf), "-sOutputFile=%s/p-%%d.pgm", dir);

  argv[na++] = (char *)"gs";
  argv[na++] = (char *)"-q";
  argv[na++] = (char *)"-dSAFER";
  argv[na++] = (char *)"-dBATCH";
  argv[na++] = (char *)"-dNOPAUSE";
  argv[na++] = wbuf;
  argv[na++] = hbuf;
  argv[na++] = (char *)"-dFIXEDMEDIA";
  argv[na++] = (char *)"-dPDFFitPage";
  argv[na++] = (char *)"-sDEVICE=pgmraw";
  argv[na++] = dpibuf;
  argv[na++] = obuf;
  argv[na++] = (char *)pdf;
  argv[na] = NULL;

  pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0)
  {
    int dn = open("/dev/null", O_RDONLY);
    int out = open("/dev/null", O_WRONLY);
    if (dn >= 0)
      dup2(dn, 0);
    if (out >= 0)
    {
      dup2(out, 1);
      dup2(out, 2);
    }
    for (int fd = 3; fd < 1024; fd++)
      close(fd);
    setsid();
    konica_child_limits(cfg);
    execvp("gs", argv);
    _exit(127);
  }
  if (wait_child(pid, cfg, cancel, cancel_ud, "gs", log, logsize) != 0)
    return -1;

  for (pages = 0; pages < 10000; pages++)
  {
    char f[768];
    struct stat st;
    snprintf(f, sizeof(f), "%s/p-%d.pgm", dir, pages + 1);
    if (stat(f, &st) != 0 || st.st_size == 0)
      break;
  }
  return pages ? pages : -1;
}

/* Minimal P5 PGM reader (8-bit only). */
static int
read_int(FILE *fp, int *out)
{
  int c, v = -1;

  for (;;)
  {
    c = fgetc(fp);
    if (c < 0)
      return -1;
    if (c == '#')
    {
      while ((c = fgetc(fp)) != '\n' && c >= 0)
        ;
      if (c < 0)
        return -1;
      continue;
    }
    if (c >= '0' && c <= '9')
    {
      v = (v < 0 ? 0 : v * 10) + (c - '0');
      continue;
    }
    if (v >= 0)
    {
      ungetc(c, fp);
      *out = v;
      return 0;
    }
    /* whitespace before any number: skip */
  }
}

static int
pgm_read(const char *path, unsigned char **px, int *w, int *h)
{
  FILE *fp = fopen(path, "rb");
  int vals[3], i;
  unsigned char *p;
  size_t need;

  if (!fp)
    return -1;
  if (fgetc(fp) != 'P' || fgetc(fp) != '5')
  {
    fclose(fp);
    return -1;
  }
  for (i = 0; i < 3; i++)
    if (read_int(fp, &vals[i]) != 0)
    {
      fclose(fp);
      return -1;
    }
  if (vals[2] != 255 || vals[0] <= 0 || vals[1] <= 0)
  {
    fclose(fp);
    return -1;
  }
  need = (size_t)vals[0] * (size_t)vals[1];
  p = malloc(need ? need : 1);
  if (!p || fread(p, 1, need, fp) != need)
  {
    free(p);
    fclose(fp);
    return -1;
  }
  fclose(fp);
  *px = p;
  *w = vals[0];
  *h = vals[1];
  return 0;
}

#include <cups/raster.h>

/* Write one page: normalize bitmap to exactly tw*th (center crop, else pad
 * white) and append a compressed CUPS raster v2 page mirroring pdftoraster's
 * header fields (8-bit gray, Duplex/Tumble, 600 dpi, zero margins). */
static int
raster_append_page(cups_raster_t *ras, const unsigned char *px, int sw, int sh,
                   int tw, int th, double pw, double ph, int res,
                   int duplex, int tumble, int media_pos)
{
  cups_page_header2_t hd;
  unsigned char *line;

  memset(&hd, 0, sizeof(hd));
  hd.cupsWidth = (unsigned)tw;
  hd.cupsHeight = (unsigned)th;
  hd.cupsBitsPerColor = 8;
  hd.cupsBitsPerPixel = 8;
  hd.cupsBytesPerLine = (unsigned)tw;
  hd.cupsColorOrder = CUPS_ORDER_CHUNKED;
  hd.cupsColorSpace = CUPS_CSPACE_W;
  hd.cupsNumColors = 1;
  hd.NumCopies = 1;
  hd.HWResolution[0] = hd.HWResolution[1] = res;
  hd.MediaPosition = (unsigned)media_pos;
  hd.Duplex = duplex ? 1 : 0;
  hd.Tumble = tumble ? 1 : 0;
  hd.PageSize[0] = (unsigned)(pw + 0.5);
  hd.PageSize[1] = (unsigned)(ph + 0.5);
  hd.ImagingBoundingBox[0] = 0;
  hd.ImagingBoundingBox[1] = 0;
  hd.ImagingBoundingBox[2] = (unsigned)(pw + 0.5);
  hd.ImagingBoundingBox[3] = (unsigned)(ph + 0.5);
  hd.cupsBorderlessScalingFactor = 1.0f;
  if (!cupsRasterWriteHeader2(ras, &hd))
    return -1;
  line = malloc(tw ? (size_t)tw : 1);
  if (!line)
    return -1;
  {
    int yoff = (sh > th) ? (sh - th) / 2 : 0;   /* crop: center */
    int xoff = (sw > tw) ? (sw - tw) / 2 : 0;
    int ypad = (th > sh) ? (th - sh) / 2 : 0;   /* pad: center on white */
    int xpad = (tw > sw) ? (tw - sw) / 2 : 0;
    int cw = sw < tw ? sw : tw;

    for (int y = 0; y < th; y++)
    {
      memset(line, 255, (size_t)tw);
      if (y >= ypad && y < ypad + (sh < th ? sh : th))
      {
        int sy = y - ypad + yoff;
        memcpy(line + xpad, px + (size_t)sy * sw + xoff, (size_t)cw);
      }
      if (cupsRasterWritePixels(ras, line, (unsigned)tw) != (unsigned)tw)
      {
        free(line);
        return -1;
      }
    }
  }
  free(line);
  return 0;
}

/* Builtin PDF renderer (KONICA_PDF_RENDERER=builtin): gs pgmraw at the PPD's
 * exact media points, normalized to pdftoraster's pixel rounding, wrapped as
 * a compressed CUPS raster v2 stream. Returns 0 on success. */
static int
render_pdf_builtin(const konica_cfg_t *cfg, const char *pdf_path, const char *ras_path,
                   const char *options, int res,
                   konica_cancel_cb cancel, void *cancel_ud, char *log, size_t logsize)
{
  char dir[600], psname[64], pat[768];
  double pw = 0, ph = 0;
  int tw, th, duplex = 0, tumble = 0, pages = 0, fd = -1;
  cups_raster_t *ras = NULL;
  int rc = -1;

  opt_pagesize(options, psname, sizeof(psname));
  if (!psname[0])
    snprintf(psname, sizeof(psname), "A4");
  if (ppd_paper_points(cfg->ppd, psname, &pw, &ph) != 0)
  {
    pw = 595.2756;
    ph = 841.8898;
  }
  if (res <= 0)
    res = 600;
  tw = (int)(pw * res / 72.0 + 0.5);   /* same rounding as pdftoraster */
  th = (int)(ph * res / 72.0 + 0.5);
  if (options && strstr(options, "DuplexNoTumble"))
  {
    duplex = 1;
    tumble = 0;
  }
  else if (options && strstr(options, "DuplexTumble"))
  {
    duplex = 1;
    tumble = 1;
  }

  snprintf(dir, sizeof(dir), "%s/konica-bi-XXXXXX", cfg->spooldir);
  if (!mkdtemp(dir))
    return -1;
  pages = gs_pdf_to_pgm(cfg, pdf_path, dir, pw, ph, res, cancel, cancel_ud, log, logsize);
  if (pages < 0)
    goto out;

  fd = open(ras_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    goto out;
  ras = cupsRasterOpen(fd, CUPS_RASTER_WRITE); /* uncompressed v2, like pdftoraster */
  if (!ras)
  {
    close(fd);
    goto out;
  }
  for (int i = 1; i <= pages; i++)
  {
    unsigned char *px = NULL;
    int sw = 0, sh = 0;
    snprintf(pat, sizeof(pat), "%s/p-%d.pgm", dir, i);
    if (cancel && cancel(cancel_ud))
      goto out;
    if (pgm_read(pat, &px, &sw, &sh) != 0 || !px)
    {
      free(px);
      if (log)
        snprintf(log + strlen(log), logsize - strlen(log), "builtin: bad pgm page %d\n", i);
      goto out;
    }
    if (raster_append_page(ras, px, sw, sh, tw, th, pw, ph, res, duplex, tumble,
                               slot_media_pos(cfg, options)) != 0)
    {
      free(px);
      goto out;
    }
    free(px);
  }
  rc = 0;

out:
  if (ras)
    cupsRasterClose(ras);
  else if (fd >= 0)
    close(fd);
  for (int i = 1; i <= pages + 1; i++)
  {
    snprintf(pat, sizeof(pat), "%s/p-%d.pgm", dir, i);
    unlink(pat);
  }
  rmdir(dir);
  return rc;
}

/* Optional capture of both sides of the vendor stage for offline diffing.
 * Set KONICA_CAPTURE_DIR to a directory; files land as <jobid>-in.ras and
 * <jobid>-out.prn. Empty/unset disables. For diagnosis only. */
static void
capture_copy(const char *src, int job_id, const char *suffix)
{
  const char *dir = getenv("KONICA_CAPTURE_DIR");
  if (!dir || !*dir || !src)
    return;
  {
    char dst[1024], cmd[2200];
    size_t i, n = 0;
    /* Keep it to a simple filename; job_id is an integer from PAPPL. */
    snprintf(dst, sizeof(dst), "%s/%d-%s", dir, job_id, suffix);
    for (i = 0; dst[i] && n + 4 < sizeof(cmd); i++)
    {
      if (dst[i] == 39) /* single quote */
      {
        memcpy(cmd + n, "'\''", 4);
        n += 4;
      }
      else
        cmd[n++] = dst[i];
    }
    cmd[n] = 0;
    if (n > 0)
    {
      char full[2300];
      snprintf(full, sizeof(full), "cp -- '%s' '%s' 2>/dev/null", src, cmd);
      if (system(full) != 0) {}
    }
  }
}

int
konica_render_raster(const konica_cfg_t *cfg, const char *raster_path, const char *out_path,
                     int job_id, const char *user, const char *title, const char *options,
                     konica_cancel_cb cancel, void *cancel_ud, char *log, size_t logsize)
{
  struct stat st;

  capture_copy(raster_path, job_id, "in.ras");
  if (konica_run_filter(cfg, cfg->vendor_filter, "application/vnd.cups-raster", "application/octet-stream",
                        job_id, user, title, options, NULL, raster_path, out_path,
                        cancel, cancel_ud, log, logsize) != 0)
    return -1;
  if (stat(out_path, &st) != 0 || st.st_size == 0)
  {
    if (log)
      snprintf(log + strlen(log), logsize - strlen(log), "vendor filter produced no output\n");
    return -1;
  }
  capture_copy(out_path, job_id, "out.prn");
  return 0;
}

int
konica_render_pdf(const konica_cfg_t *cfg, const char *pdf_path, const char *out_path,
                  int job_id, const char *user, const char *title, const char *options,
                  konica_cancel_cb cancel, void *cancel_ud, char *log, size_t logsize)
{
  char dir[600], raster[700];
  int rc = -1;

  snprintf(dir, sizeof(dir), "%s/konica-XXXXXX", cfg->spooldir);
  if (!mkdtemp(dir))
  {
    if (log)
      snprintf(log + strlen(log), logsize - strlen(log), "mkdtemp(%s): %s\n", dir, strerror(errno));
    return -1;
  }
  snprintf(raster, sizeof(raster), "%s/job.ras", dir);

  if (cfg->pdf_renderer)
  {
    /* Builtin path (KONICA_PDF_RENDERER=builtin): gs pgmraw at the PPD's
     * exact media points, normalized and wrapped as CUPS raster v2.
     * No system cups-filters binary involved. */
    int res = 600;
    if (options)
    {
      int rx = 0, ry = 0;
      const char *pr = strstr(options, "printer-resolution=");
      if (pr && sscanf(pr + 19, "%dx%d", &rx, &ry) == 2 && rx > 0 && ry > 0)
        res = rx;
    }
    if (render_pdf_builtin(cfg, pdf_path, raster, options, res,
                           cancel, cancel_ud, log, logsize) != 0)
      goto out;
  }
  else if (konica_run_filter(cfg, cfg->raster_filter, "application/pdf", "application/vnd.cups-raster",
                             job_id, user, title, options, pdf_path, NULL, raster,
                             cancel, cancel_ud, log, logsize) != 0)
  {
    /* Stage 1: PDF -> CUPS raster, geometry/colorspace taken from the PPD. */
    goto out;
  }

  /* Stage 2: Konica vendor raster filter, linked against its private libcups. */
  if (konica_render_raster(cfg, raster, out_path, job_id, user, title, options, cancel, cancel_ud, log, logsize) != 0)
    goto out;
  rc = 0;

out:
  unlink(raster);
  rmdir(dir);
  return rc;
}

/* ---- option mapping ---------------------------------------------------- */

static const struct { const char *pwg; const char *ppd; } pagesizes[] =
{
  { "iso_a3_297x420mm",  "A3" },     { "iso_a4_210x297mm",  "A4" },
  { "iso_a5_148x210mm",  "A5" },     { "iso_a6_105x148mm",  "A6" },
  { "iso_b4_250x353mm",  "B4" },     { "jis_b4_257x364mm",  "B4" },
  { "iso_b5_176x250mm",  "B5" },     { "jis_b5_182x257mm",  "B5" },
  { "na_letter_8.5x11in","Letter" }, { "na_legal_8.5x14in", "Legal" },
  { "na_executive_7.25x10.5in", "Executive" },
  { "na_invoice_5.5x8.5in", "Statement" },
  { NULL, NULL }
};

/* Does the PPD define "*<ui> <name>/..." ? (e.g. PageSize, InputSlot) */
static int
ppd_has_option(const char *ppd_path, const char *ui, const char *name)
{
  FILE *fp = fopen(ppd_path, "r");
  char line[512], key[128];
  int found = 0;

  if (!fp)
    return 0;
  snprintf(key, sizeof(key), "*%s %s", ui, name);
  while (fgets(line, sizeof(line), fp))
  {
    size_t kl = strlen(key);
    if (!strncmp(line, key, kl) && (line[kl] == '/' || line[kl] == ':' || line[kl] == ' '))
    {
      found = 1;
      break;
    }
  }
  fclose(fp);
  return found;
}

/* IPP tray source -> PPD InputSlot. Only trays the driver advertises
 * (auto, tray-1, by-pass-tray) are mapped; anything else is dropped so the
 * vendor filter never sees an unknown slot. */
static const struct { const char *pwg; const char *ppd; } sources[] =
{
  { "auto",         "Auto" },
  { "tray-1",       "Tray1" },
  { "by-pass-tray", "Bypass" },
  { NULL, NULL }
};

void
konica_build_options(const konica_cfg_t *cfg, char *out, size_t outsize,
                     const char *pwg_media_name, const char *pwg_source_name,
                     const char *sides, int xres, int yres)
{
  size_t used = 0;
  const char *duplex = "None";

  out[0] = '\0';

  if (pwg_media_name)
    for (int i = 0; pagesizes[i].pwg; i++)
      if (!strcmp(pwg_media_name, pagesizes[i].pwg))
      {
        if (ppd_has_option(cfg->ppd, "PageSize", pagesizes[i].ppd))
          used += snprintf(out + used, outsize - used, "PageSize=%s ", pagesizes[i].ppd);
        break;
      }

  if (pwg_source_name)
    for (int i = 0; sources[i].pwg; i++)
      if (!strcmp(pwg_source_name, sources[i].pwg))
      {
        if (ppd_has_option(cfg->ppd, "InputSlot", sources[i].ppd))
          used += snprintf(out + used, outsize - used, "InputSlot=%s ", sources[i].ppd);
        break;
      }

  /* sides -> Duplex / Tumble, per the plan */
  if (sides && !strcmp(sides, "two-sided-long-edge"))
    duplex = "DuplexNoTumble";
  else if (sides && !strcmp(sides, "two-sided-short-edge"))
    duplex = "DuplexTumble";
  used += snprintf(out + used, outsize - used, "Duplex=%s sides=%s ", duplex, sides ? sides : "one-sided");

  if (xres > 0 && yres > 0)
    used += snprintf(out + used, outsize - used, "Resolution=%dx%ddpi printer-resolution=%dx%ddpi ", xres, yres, xres, yres);

  if (used && out[used - 1] == ' ')
    out[used - 1] = '\0';
}
