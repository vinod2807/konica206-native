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
    /* Contain hostile/corrupt inputs: no core dumps, bounded CPU/address
     * space/output size. Limits come from KONICA_MAX_MEM_MB /
     * KONICA_MAX_FILE_MB (0 disables that limit). A4 600 dpi gray is
     * ~35 MB/page, so the 2048/512 MiB defaults have wide headroom. */
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

int
konica_render_raster(const konica_cfg_t *cfg, const char *raster_path, const char *out_path,
                     int job_id, const char *user, const char *title, const char *options,
                     konica_cancel_cb cancel, void *cancel_ud, char *log, size_t logsize)
{
  struct stat st;

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

  /* Stage 1: PDF -> CUPS raster, geometry/colorspace taken from the PPD. */
  if (konica_run_filter(cfg, cfg->raster_filter, "application/pdf", "application/vnd.cups-raster",
                        job_id, user, title, options, pdf_path, NULL, raster,
                        cancel, cancel_ud, log, logsize) != 0)
    goto out;

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
