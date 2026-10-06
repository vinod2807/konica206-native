# konica206-native: Hardening & Long-Term Maintenance Guide

Version 1.0, 2026-10-06. Builds on `konica206-native-pappl-plan.md` (original design) and
`konica206-native-hardening-plan.md` (live status + proposed steps). This document merges both, adds the
review findings, and turns them into phased work with acceptance criteria, tests and rollback for each phase.

Audience: the owner, and any AI coding agent doing the work. Agents: follow **Section 2 (rules)** and stop
at every **GATE**.

> Items marked **[VERIFY]** are proposals or assumptions that have not been tested on the real machine.
> Do not treat them as facts.

---

## Contents
1. [Where things stand](#1-where-things-stand)
2. [Rules of engagement](#2-rules-of-engagement)
3. [Architecture: today and target](#3-architecture-today-and-target)
4. [Risk register](#4-risk-register)
5. [Roadmap overview and order](#5-roadmap-overview-and-order)
6. [Phase 0: baseline and safety net](#phase-0-baseline-and-safety-net)
7. [Phase 1: operating pieces](#phase-1-operating-pieces)
8. [Phase 2: CUPS 3 compile pass](#phase-2-cups-3-compile-pass)
9. [Phase 3: own the PDF-to-raster step](#phase-3-own-the-pdf-to-raster-step)
10. [Phase 4: dependency vendoring](#phase-4-dependency-vendoring)
11. [Phase 5: functional gaps](#phase-5-functional-gaps)
12. [Phase 6: CI, release, documentation](#phase-6-ci-release-documentation)
13. [Test strategy](#13-test-strategy)
14. [Rollback matrix](#14-rollback-matrix)
15. [Open decisions](#15-open-decisions)
16. [Appendices](#16-appendices)

---

## 1. Where things stand

### 1.1 What exists and is proven (per the owner's 2026-10-06 status)
- Native PAPPL app on **port 8001**, printer `konica206`, state in `/var/lib/konica206-native.state`.
- Separate CUPS proxy queue `konica206-native` -> `ipp://localhost:8001/ipp/print/konica206`
  (A4 / one-sided defaults, custom PPD `dist/konica206-native.ppd`, ISOB4/ISOB5 removed for Wine/Adobe).
- Old stack (`konica206uri`, port 8000, and classic `KONICA_MINOLTA_206`) running untouched as rollback.
- Paper-tested: A4 simplex, multi-page simplex, duplex long and short edge, Letter, copies, PNG/JPEG,
  qpdfview, Adobe Reader through the proxy queue.
- 8192-byte USB chunking; in-app stuck-job watchdog (`KONICA_STUCK_SECS`, default 600 s).
- Build toolchain intentionally removed from the machine. Rebuilding needs a temporary install of
  `build-essential pkg-config libpappl-dev libusb-1.0-0-dev libcups2-dev`. Runtime `libpappl1t64` must stay.

### 1.2 What is not done
- No systemd auto-start, no udev rule installed (files exist in `dist/` only).
- PDF rendering shells out to the **system** `pdftoraster` (cups-filters).
- `libpappl` and `libcups` are dynamically linked from the distro.
- No compile pass against CUPS 3 headers. Tray names (`tray-1`, `by-pass-tray`) are unconfirmed against the PPD.
- PostScript is not advertised, and rejection is not user-friendly.
- No log rotation or state backup.

### 1.3 Current pipeline
```
IPP PDF  -> pdftoraster (SYSTEM cups-filters) -> 245igdirf (private libs) -> libusb bulk <=8192 B -> printer
IPP PWG/URF/JPEG/PNG -> PAPPL raster -> CUPS raster v2 (compressed) -> 245igdirf -> libusb -> printer
```

### 1.4 Known facts about the hardware and driver (carry these into every decision)
- USB `132b:232b`, serial `A8A6041029423`, **interface 1** is the printer-class interface.
- A single large USB write (~61 KB) can make the printer drop off the bus. Chunk at 8192. Never "optimise" this.
- `*DefaultOCM_TonerSave: TRUE` must be present in the PPD the vendor filter reads, otherwise pages print solid black.
- Banner / test-page PDFs (`application/vnd.cups-pdf-banner`) wedge the printer. Never send them through this queue.
- `245igdirf` is closed source and must run with its **private** `libcups.so.2`/`libcupsimage.so.2`
  (`/usr/local/lib/konica/lib`). The driver tree lives under `/usr/local/lib/konica` because the distro package is gone.
- PAPPL 1.3 requires all five raster callbacks even for a PDF-first driver, and silently drops driver data that fails
  validation (symptom: printer with no `media-supported`, `sides-supported`, etc.).

---

## 2. Rules of engagement

1. **One change at a time, each on its own branch**, paper-tested before merge, previous binary kept for instant rollback.
2. **Never touch the rollback stack**: `konica206uri` (port 8000), `KONICA_MINOLTA_206` queue, `/etc/cups`,
   `/var/lib/legacy-printer-app`, `/usr/local/lib/konica`. Read-only access only.
3. **USB interface 1 has one owner at a time.** Before any physical test, make sure only one stack is running,
   and say which one.
4. **Ask before anything that uses paper, needs `sudo`, restarts a service, or changes the default printer.**
5. **No generic "improvements" in the USB path.** Chunk size, timeouts and retry limits change only with a reason
   and a paper test.
6. **Every phase ends with the rollback check** (Section 14): old queues print normally.
7. **Report, don't guess.** If a path, option name or behaviour differs from this guide, stop and report.
8. Do not send banner/test-page jobs. Do not enable CUPS `job-sheets`.

---

## 3. Architecture: today and target

### 3.1 Today
```
client / CUPS proxy queue
        | IPP 2.0 (port 8001, all interfaces: confirmed in source,
        | `papplSystemAddListeners(system, NULL)` in `src/main.c`)
        v
konica206-native (PAPPL, libpappl + libcups dynamic from distro)
  |-- PDF  : fork/exec system pdftoraster  --\
  |-- RASTER (PWG/URF/JPEG/PNG): own CUPS raster writer --> 245igdirf (private libs) --> spool file
  |-- libusb (dynamic), <=8192 B chunks, 10 s per-transfer timeout,
  |   6 consecutive zero-progress stalls max (both confirmed in
  |   `src/konica_usb.h`: `KONICA_TIMEOUT_MS`, `KONICA_MAX_STALLS`)
  v
bizhub 206
```

### 3.2 Target
```
client / CUPS proxy queue
        | IPP 2.0, localhost only unless deliberately widened
        v
konica206-native  (only libc + libusb dynamic from the distro; PAPPL/libcups vendored or static)
  |-- PDF : OWNED rasterizer (see Phase 3) --> CUPS raster --\
  |-- RASTER path (unchanged)                                 >--> 245igdirf (private libs, resource-limited child)
  |-- libusb, <=8192 B chunks, watchdog, clean abort on unplug
  v
bizhub 206
```
Design goal: after Phase 4, `ldd konica206-native` lists nothing a distro upgrade is likely to remove or rename
except libc and libusb.

---

## 4. Risk register

Severity: High = prints stop after a plausible distro/CUPS change. Medium = needs rework later. Low = convenience.

| # | Risk | Failure mode | Sev | Addressed in |
|---|---|---|---|---|
| R1 | System `pdftoraster` (cups-filters) | CUPS 3 distro renames or removes the filter or its path; every PDF job fails | High | Phase 3 |
| R2 | Dynamic `libcups.so.2` (our binary and `libpappl`) | CUPS 3 ships `libcups3`; loader fails at startup | High | Phase 2, 4 |
| R3 | Dynamic `libpappl` | PAPPL 2.0 ABI break or package removal | Medium | Phase 4 |
| R4 | Closed `245igdirf` + private libcups | Kernel/libc drift; unpatchable | Medium-low (no alternative) | Phase 0 (backup, checksums) |
| R5 | `libusb-1.0` | Removal (very unlikely) | Low | accept |
| R6 | CUPS proxy queue built with `lpadmin -P` + custom PPD | CUPS 3 drops PPD queue creation; queue must become PPD-less IPP | Low | Phase 5 |
| R7 | **IPP listener on all interfaces** (`papplSystemAddListeners(system, NULL)` in the original code) | Anyone on the LAN can submit jobs or open the web UI | Medium on a shop network | Phase 1 |
| R8 | Watchdog vs long healthy jobs | 600 s limit cancels a legitimately slow job (large PDF at 600 dpi, printer warm-up) | Medium | Phase 1 |
| R9 | Malformed/hostile PDF | `pdftoraster`/Ghostscript hang or exhaust memory/disk; queue stalls or box swaps | Medium | Phase 1, 3 |
| R10 | No state/log management | Lost printer registration; unbounded log growth | Low | Phase 5 |
| R11 | PAPPL idle status polling opens/closes the USB device | Printer misbehaves when idle for long periods [VERIFY] | Medium | Phase 1 (observe) |
| R12 | Vendor filter licensing | Cannot redistribute `245igdirf` | n/a (personal use) | Phase 6 docs |

---

## 5. Roadmap overview and order

Recommended order (changed from the original: CUPS 3 compile pass moves up because it is cheap and decides how urgent Phases 3 and 4 are):

| Order | Phase | Goal | Risk to printing | Effort |
|---|---|---|---|---|
| 1 | Phase 0 | Baselines, backups, golden outputs | none | small |
| 2 | Phase 1 | systemd, udev, localhost binding, rlimits, watchdog check | low | small |
| 3 | Phase 2 | Compile against CUPS 3 headers, fix guards | none (build only) | small-medium |
| 4 | Phase 3 | Remove dependency on system `pdftoraster` | **highest** (changes pixels) | medium-large |
| 5 | Phase 4 | Vendor/static-link PAPPL and libcups | medium | medium |
| 6 | Phase 5 | Tray mapping, PostScript rejection, logs, state backup, CUPS-3 proxy queue | low | small |
| 7 | Phase 6 | CI matrix, releases, docs | none | small |

Each phase below has the same layout: **Goal, Why, Tasks, Acceptance, Tests, Rollback**.

---

## Phase 0: baseline and safety net

**Goal:** be able to prove any later change did not alter printed output, and recover any lost component.

**Tasks**
1. Checksummed backups, stored off the machine as well:
   ```sh
   sudo tar czf konica-driver-$(date +%F).tar.gz /usr/local/lib/konica /var/lib/legacy-printer-app/ppd
   sha256sum konica-driver-*.tar.gz > konica-driver-$(date +%F).sha256
   sudo cp /var/lib/konica206-native.state ~/konica206-native.state.$(date +%F)
   ```
2. Archive the currently running binary and the exact git commit/tag it came from
   (live binary runs from the repo checkout, not an installed copy):
    `cp /root/konica206-native/konica206-native ~/konica206-native.known-good && sha256sum ...`
3. Record the environment: distro, kernel, `pappl`/`cups`/`cups-filters`/`libusb` versions, resolved filter paths,
   `ldd` output for the binary and for `245igdirf` (with `LD_LIBRARY_PATH=/usr/local/lib/konica/lib`).
4. **Golden outputs.** Pick a small fixed set of test documents (see 13.3). For each, run the current pipeline
   with `konica-render` (no printer involved) and store the output plus its SHA-256 and size.
   Caveat **[VERIFY]**: the vendor stream may embed job id, user name, title or timestamps. Run the same job twice,
   diff the two outputs, and write down which byte ranges differ. Later comparisons must mask those ranges, or fix
   the job id/user/title passed to the filter in test runs (the `konica-render` tool already uses fixed values).
5. Record **page-level visual references**: print each golden document once on paper, scan or photograph it, and keep
   it. Byte-identical output is the strict check, visual identity is the fallback when the rasterizer changes
   (Phase 3).

**Acceptance:** backup tarball restores into a scratch directory and `245igdirf` runs from it
(`LD_LIBRARY_PATH=<scratch>/lib ldd` resolves); golden table committed to the repo.
**Rollback:** nothing to roll back (read-only).

**GATE 0:** owner confirms backups are stored somewhere other than the shop machine.

---

## Phase 1: operating pieces

**Goal:** the app starts at boot, survives replug, is not exposed to the LAN, and cannot be wedged by one bad job.

### 1.1 Install service, udev rule, binary
- `dist/konica206-native.service` -> `/etc/systemd/system/`
- `dist/70-konica206.rules` -> `/etc/udev/rules.d/` (`sudo udevadm control --reload && sudo udevadm trigger`)
- Binary -> `/usr/local/bin/konica206-native` (keep `.known-good` copy from Phase 0).
- Do not enable the unit until the old `konica206uri` stack and the new one are not both claiming interface 1
  (decide which one auto-starts; **[DECISION D1]**).

### 1.2 Bind to localhost
The original code listens on all interfaces. For a shop machine used only through a local proxy queue:
```c
/* src/main.c, system_cb(): replace papplSystemAddListeners(system, NULL) */
papplSystemAddListeners(system, "localhost");   /* [VERIFY] accepted name; fall back to "127.0.0.1" */
```
Make it configurable (`-o listen=any|localhost`, default localhost) so a LAN-visible printer is a conscious choice.
Verify: `ss -ltnp | grep 8001` shows `127.0.0.1:8001` (and `[::1]`), and a connection from another host is refused.
If DNS-SD advertisement is not wanted on localhost-only mode, leave it off (it failed in the sandbox with no daemon,
which is harmless).

### 1.3 Child-process resource limits (R9)
Add to the filter child in `konica_run_filter()` just before `execve`:
```c
struct rlimit rl;
rl.rlim_cur = rl.rlim_max = 0;                         setrlimit(RLIMIT_CORE, &rl);
rl.rlim_cur = rl.rlim_max = cfg->timeout + 30;         setrlimit(RLIMIT_CPU,  &rl);
rl.rlim_cur = rl.rlim_max = (rlim_t)cfg->max_mem_mb << 20; setrlimit(RLIMIT_AS, &rl);   /* default e.g. 2048 MiB [VERIFY] */
rl.rlim_cur = rl.rlim_max = (rlim_t)cfg->max_file_mb << 20; setrlimit(RLIMIT_FSIZE, &rl); /* default e.g. 512 MiB */
```
Rationale: an A4 gray page at 600 dpi is about 35 MB raw; multi-page spool files are not fully in memory, but
a hostile or corrupt PDF can blow up. Tune the defaults from measured peak usage on the golden set
(`/usr/bin/time -v`). New env vars: `KONICA_MAX_MEM_MB`, `KONICA_MAX_FILE_MB`.

### 1.4 Watchdog review (R8)
Questions to answer by reading the watchdog code and testing:
- What counts as "silent"? It must be progress-based (bytes written to USB, filter output growing, stage change),
  not wall-clock since job start.
- A 60-page 600 dpi job: confirm it is not cancelled mid-way.
- What happens to USB state when the watchdog cancels during a bulk write? The write has a 10 s timeout, so cancellation
  should happen between chunks. Confirm the printer is reusable afterwards without a power cycle.
Acceptance: a deliberately slow job (e.g. `sleep` stage via a test filter) is cancelled at the configured limit; a long
real job is not.

### 1.5 Observe idle behaviour (R11)
Leave the service running and idle for a day. Check `dmesg | grep -i usb` for resets and disconnects, and confirm the
first job of the morning prints. If the printer drops off the bus during idle polling, options are: increase the PAPPL
status interval, make `dev_status` avoid opening the device, or only open the device when a job exists **[VERIFY: PAPPL
1.3 behaviour]**.

### 1.6 systemd hardening (optional, test carefully)
```ini
[Service]
NoNewPrivileges=yes
ProtectSystem=strict
ReadWritePaths=/var/lib /var/tmp
ProtectHome=yes
PrivateTmp=yes
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6 AF_NETLINK
RestrictNamespaces=yes
LockPersonality=yes
MemoryDenyWriteExecute=no        # Ghostscript/the vendor filter may need it [VERIFY]
Restart=on-failure
RestartSec=3
```
Do not set `PrivateDevices=yes` (the app needs `/dev/bus/usb`). Add one directive at a time and print after each.

**Acceptance (Phase 1):**
- Reboot -> printing works with no manual start.
- Unplug/replug -> next job prints without restarting the service.
- Port 8001 not reachable from another host.
- Corrupt PDF test (13.4) fails the job cleanly in < timeout and the next job prints.
- Old queues still print (rollback check).

**Rollback:** `systemctl disable --now konica206-native`, delete the rule, restart the old stack.

---

## Phase 2: CUPS 3 compile pass

**Goal:** find out now, while CUPS 2 output is still available for comparison, what breaks on CUPS 3.

**Tasks**
1. Build container/VM with CUPS 3 headers and a PAPPL that targets CUPS 3 (OpenPrinting repos; version choice **[VERIFY]**).
   Keep it entirely separate from the shop machine.
2. Build as-is. Expect breakage around: `cups_len_t`, `cupsRasterWriteHeader` vs `cupsRasterWriteHeader2`
   (guards exist in `src/main.c`), `cups_page_header_t` vs `_header2_t`, deprecated option APIs, `ipp.h` changes.
3. Fix with `CUPS_VERSION_MAJOR` guards, never by deleting the CUPS 2 path.
4. Run `make check` and the file-device IPP job test on both builds. For the raster path, compare the CUPS 3 build's
   raster output with the CUPS 2 build's for the same PNG (header fields and pixel data).
5. Add a CI matrix (Phase 6) so both stay green.

**Decision after this phase:** if CUPS 3 needs only guard fixes and the distro ships `libcups3` alongside `libcups2`
for a long time, Phases 3 and 4 become less urgent. If the PAPPL package itself moves to CUPS 3 on the shop machine's
distro, Phase 4 becomes mandatory.

**Acceptance:** green build on both header sets; no change in CUPS 2 output (golden table).
**Rollback:** none needed (build-only, branch only).

---

## Phase 3: own the PDF-to-raster step

**Goal:** a PDF job no longer depends on `cups-filters` being present, named or located the same way.

### 3.1 Options

| | A. In-process `libcupsfilters` | B. Ship a private `pdftoraster` + libs | C. Direct render with Ghostscript or poppler, reuse our raster writer |
|---|---|---|---|
| Removes cups-filters dependency | Partly (library vendored) | Yes (private copy) | **Yes** |
| New dependency | libcupsfilters + its deps, vendored | A vendored tree to maintain | `gs` or `pdftoppm` (stable, not CUPS-versioned) |
| Code change | Large: needs filter-data/PPD setup, API differs 1.x vs 2.x | Small | Medium: argument building, geometry, duplex |
| Output identical to today | Yes (same code) | Yes (same code, if same version) | **No** (must be proven or tuned) |
| Fork/exec boundary | none | yes | yes |
| Ties to CUPS ecosystem | Strong | Strong | None |
| Behaviour on gs bug (see note) | n/a | n/a | avoids the `gstoraster`/`cupsICCProfile` class of failures seen earlier [VERIFY] |

### 3.2 Recommendation
1. **Prototype C first**, because it removes the dependency for good and the raster back half already works.
2. If C cannot reproduce today's output closely enough (3.4), fall back to **B** (lowest risk, smallest change).
3. Treat A as a last resort: highest complexity, still tied to CUPS releases.

### 3.3 Design of option C **[VERIFY all of this]**
```
PDF --(pdftoppm -gray -r RES -f P -l P)  or  (gs -sDEVICE=pgmraw -r RES ...)--> 8-bit gray page bitmaps
    --> our CUPS raster v2 writer (same as the PNG path: header, geometry, Duplex/Tumble)
    --> 245igdirf --> spool --> USB
```
Points that decide whether C is equivalent to `pdftoraster`:
- **Page geometry.** Today `pdftoraster` takes size and imageable area from the PPD (`KonicaMinolta-206-fullbleed.ppd`:
  full page, no margins). The direct path must produce a bitmap of exactly `width_pts*dpi/72 x height_pts*dpi/72`
  pixels (A4 600 dpi: 4960 x 7015, as seen in the PNG path) with the page content scaled/centered as today.
  Pick the page size from the IPP media (not from the PDF) when `print-scaling` is `fit`/`auto`; honour `fill`/`none`.
- **Scaling/rotation.** Handle `orientation-requested` and PDFs whose page is landscape. `pdftoraster`/`pdftopdf`
  semantics should be matched or consciously simplified.
- **Duplex back-side orientation.** The working setup uses "rotated backside" handling. Find where it lives
  (PPD `cupsFlipDuplex`, the vendor filter, or `pdftoraster`). The raster header already carries `Duplex`/`Tumble`;
  prove on paper that long-edge and short-edge back sides come out right with the new path.
- **Anti-aliasing and halftone.** `pdftoraster` may apply dithering/gamma. The vendor filter likely does its own
  halftoning from 8-bit gray, so pixel-exactness is not expected. Judge by print comparison (3.4).
- **Rendering engine fidelity.** Poppler and Ghostscript differ from each other on fonts and transparency.
  Today's renderer is cups-filters' `pdftoraster`, which uses poppler (with `pdftopdf` in the CUPS pipeline).
  Using poppler (`pdftoppm`) for C is the closest match. Ghostscript is the fallback if poppler is unavailable.
- **Memory/time at 600 dpi.** About 35 MB per A4 gray page; render and stream page by page rather than holding
  the document.
- **Colour PDFs and images.** Force gray (`-gray`) and `print-color-mode=monochrome`.

### 3.4 Acceptance for Phase 3
Run the golden set (13.3) through old and new rasterizers:
1. Page count and page size identical for every document.
2. Mechanical comparison of the two raster streams: header fields identical except those listed as acceptable;
   per-page image difference measured (e.g. mean absolute difference, % pixels differing by > N levels).
   Define thresholds before testing **[DECISION D2]**.
3. **Paper comparison**, side by side, for: text-heavy page, small fonts, thin lines, photo, transparency, barcode/QR,
   landscape page, mixed page sizes, duplex long and short edge, Letter, A5.
4. Wine/Adobe regression: print from Adobe Reader through the proxy queue.
5. Remove or rename the system `pdftoraster` on the test machine and confirm jobs still print.
6. Corrupt-PDF and huge-PDF tests stay clean (Phase 1.3 limits apply).

### 3.5 Rollout
Feature flag `KONICA_PDF_RENDERER=system|builtin` (default `system` until the paper tests pass), so the old path remains
one environment variable away. Switch the default only after two weeks of normal use without regression, and keep the
`system` code path for at least one release.

**Rollback:** `KONICA_PDF_RENDERER=system` and restart; or reinstall `.known-good` binary.

**GATE 3:** owner signs off on the paper comparison.

---

## Phase 4: dependency vendoring

**Goal:** `ldd konica206-native` shows nothing the distro is likely to remove or rename, except libc and libusb.

### 4.1 Why `libpappl` and `libcups` move together
`libpappl` links `libcups`. Vendoring PAPPL alone leaves the `libcups.so.2` dependency (R2). Our own code also calls
libcups (raster writer, option parsing). So the vendored set is **libpappl + the libcups it was built against + their
non-system deps** (libpng, libjpeg, libssl/gnutls, libavahi-client if kept **[VERIFY dependency list with `ldd`]**).
The vendor filter's private libcups lives in a different process and path and is unaffected.

### 4.2 Approach
- **Preferred: private lib directory with rpath**, mirroring `/usr/local/lib/konica/lib`:
  `/usr/local/lib/konica206-native/lib/` containing the vendored `.so` files; link with
  `-Wl,-rpath,/usr/local/lib/konica206-native/lib` (or `$ORIGIN/../lib/konica206-native`).
- **Alternative: static link** (`libpappl.a`, `libcups.a`) where licences permit. PAPPL and CUPS are Apache-2.0 with
  the GPL2/LGPL2 exception, so redistribution is fine, but record versions and licence texts in the repo.
- Build the vendored libraries from pinned release tarballs (record URL + SHA-256 in `vendor/README.md`), in a clean
  container, with the same flags each time.

### 4.3 Acceptance
- `ldd konica206-native` lists only libc, libm, libpthread, libusb (and whatever is unavoidable, documented).
- `readelf -d konica206-native | grep -E 'RPATH|RUNPATH'` shows the private path.
- Uninstalling the distro `libpappl1t64` on a **test** machine does not break the app.
- Full paper test matrix (13.2) passes.
- Security note: vendored libraries do not receive distro security updates. Add a calendar reminder or CI job to
  check upstream PAPPL/CUPS release notes quarterly **[DECISION D3]**.

**Rollback:** reinstall previous binary (dynamic) and `libpappl1t64` stays installed throughout.

---

## Phase 5: functional gaps

### 5.1 Tray-name mapping
1. Read the real names: `grep -E '^\*(InputSlot|DefaultInputSlot)' "$KONICA_PPD"`.
2. Add a small table `source -> InputSlot=<name>` in `konica_build_options()` (same pattern as page sizes, only emitted if
   the PPD defines it).
3. Paper test: tray 1 and bypass tray, with the "wrong" tray empty to prove selection works. Record what the printer
   does when the chosen tray is empty (does it prompt, or wait?) and make the IPP state reflect it if possible.
4. Update `media-source-supported`/`media-ready` if the printer can only report those statically.

### 5.2 PostScript and other unsupported inputs
PAPPL 1.3 has no PostScript path. Options: (a) leave unadvertised and make rejection explicit, (b) accept
`application/postscript` and convert with `ps2pdf`/Ghostscript to PDF in the job callback (adds the Ghostscript
dependency and the Wine/Adobe MIME issue noted earlier). Recommended: (a) now, (b) only if a real client needs it.
For (a): return a human-readable `status-message` (e.g. "PostScript not supported; print as PDF") on
`document-format-not-supported`, and log the format for diagnosis.

### 5.3 Logging and rotation
- Under systemd, log to stderr and let journald rotate (`SystemMaxUse=`). This is the simplest answer.
- If a file log is kept (`-o log-file=`), add `/etc/logrotate.d/konica206-native` with weekly rotation, 8 kept,
  `copytruncate` (the app does not reopen files on SIGHUP **[VERIFY]**).
- Keep PAPPL's own log level at `info` in production; `debug` only while testing (it logs every IPP attribute).
- Note: on the distro build tested, PAPPL's own log lines print blanks for `%d` values. Convert numbers to strings
  before logging (the app already does this for `copies`).

### 5.4 State backup/restore
- Nightly copy of `/var/lib/konica206-native.state` (and the printer URI) via a systemd timer or cron, 7 days kept.
- Document the restore: stop service, copy file back, start, `konica206-native printers`.
- Document the "from scratch" path: `add -d konica206 -m konica-bizhub-206 -v 'konica://KONICA%20MINOLTA/206?serial=...'`.

### 5.5 CUPS proxy queue and CUPS 3
Today the proxy queue already uses the shipped PPD (`lpadmin -p konica206-native -P dist/konica206-native.ppd`,
real size names, ISOB4/ISOB5 removed for Wine — verified live 2026-10-06).
In CUPS 3, `lpadmin -P` and `driverless` are expected to remain for a while but are deprecated **[VERIFY with the
CUPS 3 build from Phase 2]**. Prepare a PPD-less variant (`-m everywhere` or plain IPP queue) and make sure the ISOB4/
ISOB5 sizes are not advertised by the app, so a regenerated queue can never bring back the Wine/Adobe problem.
Add a check to the test plan: after any queue regeneration, `lpoptions -p konica206-native -l | grep -i isob` must be empty.

### 5.6 Smaller items
- Identify-Printer support (optional).
- Printer-state reasons: set `offline`/`media-empty` where the printer exposes them over the USB back-channel
  **[VERIFY: GDI printers often expose little]**. Otherwise leave.
- Live app log location (2026-10-06): the running instance was started with stdout/stderr redirected to
  `/tmp/konica206-native.log`, not journald. Any log-rotation plan must first decide the real log destination.
- Document the `submit -n` limitation of the built-in CLI (copies not forwarded).

**Acceptance (Phase 5):** each item has a paper or log test recorded in the repo.

---

## Phase 6: CI, release, documentation

- **CI matrix** (GitHub Actions or similar): Ubuntu with CUPS 2.4 + PAPPL 1.3, a CUPS 3 container, with `make`,
  `make check`, and the file-device IPP test (needs PAPPL, fake filters, no printer).
- **Static checks:** `-Wall -Wextra` clean; optionally `cppcheck`/`clang-tidy`; run the chunking tests under
  AddressSanitizer/UBSan.
- **Fuzz-ish tests:** random/truncated PDFs, zero-byte files, huge page sizes through `konica-render` with the real or
  stand-in filters, asserting "fails cleanly, no leftovers in the spool dir, no zombie processes".
- **Releases:** semantic tags; each release attaches the binary, its SHA-256, and the vendored library list. Keep the
  last two release binaries on the machine.
- **Docs to maintain:** `README.md` (quick start), `INSTALL.md` (agent-oriented install, already written),
  this guide, `CHANGELOG.md`, `docs/hardware-notes.md` (the facts in 1.4), `docs/rollback.md`.
- **Licence file:** state that the Konica filter and driver files are not part of this repository and are not
  redistributed.

---

## 13. Test strategy

### 13.1 Levels
| Level | Needs | Run when |
|---|---|---|
| L0 unit: `make check` (fake libusb) | nothing | every commit |
| L1 offline pipeline: `konica-render` with real or stand-in filters | filters | every change to filter/option code |
| L2 IPP no paper: PAPPL + `konicafile://` device | PAPPL, filters | every change to `main.c` |
| L3 USB no paper: `konica-send` of a stored stream | printer (prints a page) | every change to `konica_usb.c` |
| L4 paper matrix | printer + paper | before merging each phase |
| L5 soak: idle day + 100-page mixed batch | printer + paper | before changing the default boot service |

### 13.2 Paper matrix (L4), one queue at a time
1. A4 simplex, 1 page
2. A4 simplex, multi-page (page order and count)
3. Duplex long-edge (sheet count, back-side orientation)
4. Duplex short-edge
5. Letter
6. A5 and A3 (if the printer supports; A3 is listed in the media table, confirm it actually exists on this model)
7. Copies = 2 and 3 (collation)
8. PNG and JPEG
9. Tray 1 and bypass tray (Phase 5.1)
10. Unplug mid-job -> app does not hang, next job prints after replug
11. Cancel mid-job via IPP Cancel-Job
12. Two jobs queued back-to-back
13. GUI paths: qpdfview, Firefox/Chromium print, LibreOffice, Adobe Reader via Wine (the ISOB regression)
14. Printer powered off, job submitted, printer powered on -> job completes (or fails cleanly)

### 13.3 Golden document set (commit under `test/golden/`, small files only)
- text page (several font sizes, including 6-8 pt)
- thin-line and table page
- photo (grayscale and colour)
- transparency/blend page
- QR code and barcode
- landscape page
- PDF with mixed page sizes
- multi-page (10) text
- Letter-size page
- a Wine/Adobe-generated PDF

### 13.4 Hostile-input tests (no paper)
- zero-byte PDF, truncated PDF, PDF with a 1,000,000-point page size, PDF with a page tree loop, a PDF that makes the
  rasterizer sleep (use a stand-in filter that sleeps) to prove timeout + cleanup.
- Assert: job aborted, state reason set, spool dir empty, no child process left, the next valid job prints.

### 13.5 Evidence to record per test
Date, binary SHA-256, phase/branch, command, result (pass/fail), and for failures the relevant log lines. Keep as
`docs/test-log.md`.

---

## 14. Rollback matrix

| Change | Rollback |
|---|---|
| Phase 1 service/udev | `systemctl disable --now konica206-native`; remove rule; start old stack |
| Binary upgrade | copy `konica206-native.known-good` back to `/usr/local/bin/`; `systemctl restart` |
| Phase 3 rasterizer | `KONICA_PDF_RENDERER=system`; restart |
| Phase 4 vendoring | reinstall previous binary; distro `libpappl1t64` is still installed |
| Phase 5 proxy queue | `lpadmin -x konica206-native`; recreate from the saved PPD |
| State corruption | restore state backup; or re-add the printer |
| Everything | stop `konica206-native`; start `konica206uri`; print a test page from `KONICA_MINOLTA_206` |

**Rollback check after every phase:** with the new stack stopped, `konica206uri` and `KONICA_MINOLTA_206` print an A4 page.

---

## 15. Open decisions

| ID | Decision | Default recommendation |
|---|---|---|
| D1 | Which stack auto-starts at boot while both exist | the old one until Phase 3 is signed off |
| D2 | Acceptable visual/numeric difference for the new rasterizer | decide with golden prints before coding |
| D3 | Who watches upstream PAPPL/CUPS security releases after vendoring | quarterly manual check or CI job |
| D4 | Keep LAN access to the printer, or localhost only | localhost only; widen with `-o listen=any` |
| D5 | PostScript support | not now |
| D6 | Keep the CUPS proxy queue on CUPS 3 or move apps to IPP-only | decide after Phase 2 |
| D7 | Public repo: include only our source, or also release binaries | decided 2026-10-06: source + binaries, never the vendor files |

---

## 16. Appendices

### A. Environment variables
| Variable | Default | Meaning |
|---|---|---|
| `KONICA_VENDOR_FILTER` | `/usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf` | vendor raster filter |
| `KONICA_RASTER_FILTER` | `/usr/lib/cups/filter/pdftoraster` | system PDF -> raster (until Phase 3) |
| `KONICA_LIBDIR` | `/usr/local/lib/konica/lib` | private libcups/libcupsimage |
| `KONICA_PPD` | `/var/lib/legacy-printer-app/ppd/KonicaMinolta-206-fullbleed.ppd` | PPD the filters read |
| `KONICA_SERIAL` | `A8A6041029423` | restrict to this printer |
| `KONICA_SPOOLDIR` | `/var/tmp` | per-job temp files |
| `KONICA_FILTER_TIMEOUT` | `300` | seconds per filter stage |
| `KONICA_STUCK_SECS` | `600` | watchdog limit (per owner's current build) |
| `KONICA_MAX_MEM_MB`, `KONICA_MAX_FILE_MB` | proposed in Phase 1.3 | child resource limits **[proposed]** |
| `KONICA_PDF_RENDERER` | proposed: `system` | `system` or `builtin` **[proposed, Phase 3]** |

### B. Useful commands
```sh
# what is the app linked against (live binary runs from the repo checkout)
ldd /root/konica206-native/konica206-native
readelf -d /root/konica206-native/konica206-native | grep -E 'NEEDED|RPATH|RUNPATH'

# vendor filter really uses the private libs
LD_LIBRARY_PATH=/usr/local/lib/konica/lib ldd /usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf | grep cups

# listener scope
ss -ltnp | grep 8001

# IPP capability check (no paper)
ipptool -tv ipp://localhost:8001/ipp/print/konica206 get-printer-attributes.test

# render only (no USB)
konica-render known.pdf /tmp/out.bin

# USB only (prints a page; stop other stack first)
konica-send /tmp/out.bin

# USB events
dmesg -w | grep -i -E 'usb|132b'
```

### C. Pre-merge checklist (copy into each PR)
- [ ] `make && make check` green on CUPS 2.4 (and CUPS 3 once Phase 2 is done)
- [ ] L2 file-device job passes
- [ ] Paper matrix items relevant to this change pass (list them)
- [ ] Golden table unchanged, or the change is explained and signed off
- [ ] Old stack rollback check passes
- [ ] Previous binary saved with SHA-256
- [ ] No change to chunk size, timeouts or retry limits (or the reason is written down)
- [ ] Docs and changelog updated

### D. Glossary
- **PAPPL**: framework for IPP Printer Applications. **IPP Everywhere**: driverless IPP standard.
- **245igdirf**: Konica's closed raster filter that turns CUPS raster into the printer's GDI stream.
- **Golden output**: a stored known-good result used to detect unintended changes.
- **Vendoring**: shipping a pinned private copy of a dependency with the application.
