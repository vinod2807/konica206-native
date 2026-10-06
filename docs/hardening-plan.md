# Hardening plan: konica206-native (reviewed for Claude AI)

## Context

`konica206-native` is a standalone PAPPL Printer Application for the
USB-connected Konica Minolta bizhub 206 (GDI-only, VID:PID `132b:232b`,
serial `A8A6041029423`, interface 1).

Proven live: A4 simplex + duplex (native IPP and CUPS proxy), Wine/Adobe
printing (after removing non-standard ISOB4/ISOB5 sizes), in-app stuck-job
watchdog, 8192-byte USB chunking. Source + binaries:
`https://github.com/vinod2807/konica206-native`

Current pipeline per job:

```text
IPP PDF -> pdftoraster (SYSTEM cups-filters) -> 245igdirf (private libs)
  -> libusb bulk, <=8192 B chunks -> printer
```

## Current status (2026-10-06)

Live and verified on the shop machine:

- Native app running on port `8001`, hardware printer `konica206` registered
  from `/var/lib/konica206-native.state`.
- Separate CUPS proxy queue `konica206-native` ->
  `ipp://localhost:8001/ipp/print/konica206`, A4 / one-sided defaults,
  using `dist/konica206-native.ppd` (real size names, ISOB4/ISOB5 removed).
- Default printer unchanged (`KONICA_MINOLTA_206`); `konica206uri` stack
  (port 8000) running untouched as rollback.
- Paper-tested: A4 simplex, multi-page simplex, duplex long-edge and
  short-edge, Letter, copies, PNG/JPEG, plus GUI apps (qpdfview) and Adobe
  Reader via the CUPS proxy queue.
- In-app stuck-job watchdog active (current binary): cancels jobs silent
  longer than `KONICA_STUCK_SECS` (default 600 s), no server restart.
- Build toolchain already removed from the machine; rebuild needs
  temporary reinstall of `build-essential pkg-config libpappl-dev
  libusb-1.0-0-dev libcups2-dev` (runtime `libpappl1t64` must stay).

Not yet done (see steps below):

- No systemd auto-start and no udev rule installed (`dist/` files only).
- PDF rendering still shells out to system `pdftoraster`.
- `libpappl`/`libcups` dynamically linked from the distro.
- No CUPS 3 compile pass yet; tray-name mapping unconfirmed.

## Dependency risks (ranked)

| # | Dependency | Failure mode | Severity |
|---|---|---|---|
| 1 | `pdftoraster` fork/exec from system `cups-filters` (`KONICA_RASTER_FILTER`) | CUPS 3 distro renames/removes the filter path; every PDF job fails | High |
| 2 | Dynamic link to system `libcups.so.2` (via PAPPL helpers) | CUPS 3 ships `libcups3`; loader fails at startup | High |
| 3 | Dynamic link to system `libpappl` | Future PAPPL 2.0 ABI break; distro package removal | Medium |
| 4 | Closed `245igdirf` + private `libcups`/`libcupsimage` in `/usr/local/lib/konica/lib` | Kernel/libc drift; unpatchable | Medium-low (no alternative exists) |
| 5 | `libusb-1.0` dynamic link | Distro removal (extremely unlikely) | Low |
| 6 | CUPS proxy queue `konica206-native` (`-m everywhere` replaced by `dist/konica206-native.ppd`) | CUPS 3 drops `lpadmin -P`; queue must become PPD-less IPP | Low (optional client convenience) |

Already mitigated: no CUPS PPD/filter/backend in the print path, private
Konica libs (rpath-patched), in-app stuck watchdog, chunked USB, A4 default,
ISOB sizes removed for Wine.

## Hardening steps (proposed order)

### Step 1 — Install the operating pieces (smallest, immediate)

Install (currently present only as uninstalled files):

- `dist/konica206-native.service` -> `/etc/systemd/system/`
- `dist/70-konica206.rules` -> `/etc/udev/rules.d/`
- binary -> `/usr/local/bin/` (keep dev copy in repo for rollback)

Verify: reboot, print without manual server start, unplug/replug permissions.
Rollback: disable unit, delete rule, restart old stack (unchanged).

### Step 2 — Own the PDF-to-raster step (biggest risk reduction)

Problem: every PDF shells out to the host's `pdftoraster`.

Options, preferred first:

- **A. In-process via `libcupsfilters` API** linked against a *bundled* copy
  (same vendoring pattern as `/usr/local/lib/konica/lib`). No fork, no
  PATH lookup, version pinned by us.
- **B. Ship a private `pdftoraster` binary + its library tree**, point
  `KONICA_RASTER_FILTER` at it. Less code change, still an exec boundary.

Acceptance: remove system `cups-filters` from the test machine (or rename
the filter path) and confirm PDF jobs still render byte-comparable output.

### Step 3 — Vendor or static-link `libpappl`

Mirror the proven Konica approach: ship `libpappl.so.1` beside the binary
with an rpath, or static-link where licensing permits. Goal: `ldd
konica206-native` shows no dependency the distro can remove except
`libusb`/libc.

### Step 4 — CUPS 3 compile pass

Build in a CUPS-3 container/VM. Exercise the existing
`CUPS_VERSION_MAJOR` guards in `src/main.c` (`cupsRasterWriteHeader` vs
`cupsRasterWriteHeader2`), fix breakage while CUPS 2 is available for
byte-level output comparison. Add CI matrix (CUPS 2.4 + CUPS 3 headers).

### Step 5 — Close functional gaps

- Tray-name mapping: `tray-1`/`by-pass-tray` vs PPD `InputSlot` names are
  guesses (see plan: "TODO(stage 4)"). Confirm against
  `grep -E '^\*InputSlot' "$KONICA_PPD"` with paper tests.
- PostScript input: currently unadvertised (PAPPL 1.3 has no PS path).
  Return an explicit, human-readable rejection instead of a generic
  format error.
- Log rotation for the app log; backup/restore of
  `/var/lib/konica206-native.state`.

## Test plan per step

1. Offline: `make && make check` (fake-USB chunking suite, must stay green).
2. File-device IPP job on isolated port (no paper, no USB).
3. Paper, one queue at a time (USB interface is exclusive):
   A4 simplex, multi-page simplex, duplex long-edge, duplex short-edge,
   Letter, copies=2, PNG/JPEG, unplug-mid-job recovery.
4. Wine/Adobe regression: print from Adobe Reader to the CUPS proxy queue
   (ISOB sizes must stay out of any regenerated PPD).
5. Rollback check after every step: old `konica206uri` + `KONICA_MINOLTA_206`
   print normally.

## Suggested execution order

Step 1, then Step 2, then Steps 3+4 together (one rebuild + retest cycle),
Step 5 last. Each step on its own branch, paper-tested before merge, with
the previous binary kept for instant rollback.
