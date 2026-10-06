# Install guide: konica206-native (for an AI coding agent)

You are installing and testing **konica206-native**, a standalone PAPPL Printer Application for a
USB-connected **Konica Minolta bizhub 206** (GDI-only). Work through the stages in order, stop at
every **STOP** marker, and report results to the user before continuing.

```
IPP client -> PAPPL -> pdftoraster -> 245igdirf (private libcups) -> libusb, 8192-byte chunks -> printer
                 \-> PWG/URF/JPEG/PNG -> CUPS raster -> 245igdirf -> libusb
```

Source: `konica206-native.tar.gz` (the user has it). The README inside has the same commands in short form.

---

## 0. Ground rules (read first)

1. **Non-destructive.** Do NOT modify, delete or restart the existing `konica206uri` PAPPL app, the
   `KONICA_MINOLTA_206` CUPS queue, `/etc/cups`, `/var/lib/legacy-printer-app`, or the vendor driver
   under `/usr/local/lib/konica`. They are the rollback path. Read them, never write to them.
2. **Separate everything.** Port **8001** (not 8000), printer name **konica206**, own state file.
3. **Never write more than 8192 bytes per USB transfer.** Larger writes knock the printer off the bus.
   The code already enforces this; do not "optimise" it.
4. **No system-wide changes before Stage 4 passes** (no systemd enable, no CUPS queue, no package holds).
   The only earlier system change allowed is the udev rule in Stage 2, and only with the user's OK.
5. **USB interface 1 has one owner.** The old `konica206uri` stack and the new app cannot hold the printer
   at the same time. Ask the user before stopping the old one, and tell them how to restore it.
6. Do not run commands that print pages without telling the user first (paper is used).
7. Banner/test-page PDFs (`application/vnd.cups-pdf-banner`) are known to wedge this printer. Never send them.

---

## 1. Prerequisites

### 1.1 Hardware / device facts
| Item | Value |
|---|---|
| Printer | Konica Minolta bizhub 206 (GDI), USB |
| USB ID | `132b:232b` |
| Serial | `A8A6041029423` (override with `KONICA_SERIAL`) |
| Printer-class USB interface | **1** |
| Device ID | `MFG:KONICA MINOLTA;CMD:GDI,XPS;MDL:206;PRINTER` |

Verify: `lsusb | grep 132b:232b` and `lsusb -v -d 132b:232b 2>/dev/null | grep -E 'bInterfaceNumber|bInterfaceClass'`

### 1.2 Files that must already exist on the machine
These come from the user's existing working setup. **Check each; if one is missing, stop and ask.**

| Purpose | Default path | Env override |
|---|---|---|
| Vendor raster filter | `/usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf` | `KONICA_VENDOR_FILTER` |
| Private libs (libcups, libcupsimage) | `/usr/local/lib/konica/lib/` | `KONICA_LIBDIR` |
| Full-bleed PPD read by the filter | `/var/lib/legacy-printer-app/ppd/KonicaMinolta-206-fullbleed.ppd` | `KONICA_PPD` |
| PDF -> CUPS raster filter | `/usr/lib/cups/filter/pdftoraster` | `KONICA_RASTER_FILTER` |

```sh
V=/usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf
test -x "$V" && echo "vendor filter ok"
# must resolve libcups/libcupsimage from the PRIVATE dir, not the system:
LD_LIBRARY_PATH=/usr/local/lib/konica/lib ldd "$V" | grep -E 'libcups'
PPD=/var/lib/legacy-printer-app/ppd/KonicaMinolta-206-fullbleed.ppd
test -r "$PPD" && echo "ppd ok"
grep -n 'DefaultOCM_TonerSave' "$PPD"     # must be TRUE, otherwise pages print solid black
```
If `ldd` shows `libcups` from `/usr/lib...` instead of `/usr/local/lib/konica/lib`, stop: the private-library
requirement is not met.

Locate the PDF->raster filter (location differs by distro / cups-filters version):
```sh
ls -l /usr/lib/cups/filter/pdftoraster /usr/libexec/cups/filter/pdftoraster 2>&1
command -v pdftoraster
```
Use whichever exists and export `KONICA_RASTER_FILTER=<path>`. If none exists, install `cups-filters`
(1.x provides `pdftoraster`; with 2.x check the package contents with `dpkg -L`/`pacman -Ql`/`rpm -ql`).
If it truly does not exist, stop and ask the user, because the PDF path needs a PDF -> CUPS-raster step.

### 1.3 Packages (build + runtime)

**Debian / Ubuntu (user runs Ubuntu 26.04)**
```sh
sudo apt install build-essential pkg-config libpappl-dev libusb-1.0-0-dev libcups2-dev \
                 cups-filters ghostscript poppler-utils usbutils curl
# ipptool for IPP tests:
sudo apt install cups-ipp-utils    # (package name may be cups-client on older releases)
```

**Arch Linux (user also runs Arch)**
```sh
sudo pacman -S --needed base-devel pkgconf pappl libusb cups cups-filters ghostscript poppler usbutils
```

**Fedora**
```sh
sudo dnf install gcc make pkgconf-pkg-config pappl-devel libusb1-devel cups-devel \
                 cups-filters ghostscript poppler-utils usbutils cups-ipptool
```
Package names drift between releases. If one is missing, search (`apt-cache search pappl`,
`pacman -Ss pappl`, `dnf search pappl`) rather than guessing.

Required library versions: **PAPPL >= 1.3** (developed against 1.3.1), libusb-1.0, libcups 2.4 (CUPS 3 is
handled via `CUPS_VERSION_MAJOR` guards but has not been compiled against).

Check what you have:
```sh
pkg-config --modversion pappl libusb-1.0 cups
gcc --version | head -1; make --version | head -1
```

---

## 2. Stage 0: build

```sh
mkdir -p ~/konica206-native && cd ~/konica206-native
tar xzf /path/to/konica206-native.tar.gz --strip-components=1
make
make check          # expect: ALL PASSED  (fake-libusb tests, needs no hardware)
```
Expected outputs: `konica206-native`, `konica-send`, `konica-render`, `test-chunking`.
If `make` fails, read the compiler error; typical causes are a missing `-dev` package or PAPPL older than 1.3.
Do not edit the source to silence errors without telling the user.

**STOP 0:** report build + `make check` results.

---

## 3. Stage 1: renderer proof (no printer interaction)

Needs a PDF the user has already printed successfully. Ask for one; do not invent one.

```sh
export KONICA_RASTER_FILTER=/usr/lib/cups/filter/pdftoraster   # adjust per 1.2
./konica-render /path/to/known-good.pdf /tmp/konica-test.bin
ls -l /tmp/konica-test.bin
```
Success = `OK: wrote N bytes`, exit code 0. The tool prints the filters, PPD, libdir and option string it used.
Compare `N` and the first bytes (`head -c 64 /tmp/konica-test.bin | od -c | head`) with a capture from the
working `konica206uri` pipeline if the user has one. Same order of magnitude and same header (PJL / GDI
signature) is a pass. Also test duplex and Letter:
```sh
./konica-render -o "PageSize=Letter Duplex=DuplexNoTumble Resolution=600x600dpi" known-good.pdf /tmp/k-duplex.bin
```
Failure output includes the filter's stderr. Common causes:
- `filter not executable` -> wrong `KONICA_RASTER_FILTER`/`KONICA_VENDOR_FILTER` path.
- Vendor filter error about CUPS libs -> private `LD_LIBRARY_PATH` not effective (see 1.2).
- `pdftoraster` complaining about the PPD -> check the PPD path and `PageSize` names.

**STOP 1:** report sizes and any filter stderr.

---

## 4. Stage 2: USB proof (no CUPS, no PAPPL)

1. Ask the user to confirm the old stack is stopped or that you may stop it. Show them what you'd run,
   e.g. `systemctl status legacy-printer-app` (or whatever unit the user has). On this user's systems the old
   app is the `konica206uri` PAPPL instance on port 8000. Remember exactly what you stopped so it can be restored.
2. udev permissions (needs sudo; ask first):
   ```sh
   sudo cp dist/70-konica206.rules /etc/udev/rules.d/
   sudo udevadm control --reload && sudo udevadm trigger
   ```
   Add the user to group `lp` if needed. Re-plug the printer or run as root for a first test.
3. Send the Stage 1 output. **This prints a page; tell the user.**
   ```sh
   ./konica-send /tmp/konica-test.bin
   ```
   Expected stderr: `opened serial=A8A6041029423 out=0x?? in=0x??` then `sent N bytes`.
4. Error meanings: `printer ... not found` = unplugged/wrong serial; `cannot open it (permissions?)` = udev rule/group;
   `claim_interface` = the old stack still owns interface 1; `printer stopped accepting data` = the printer needs a power cycle.

**STOP 2:** report whether the page printed correctly.

---

## 5. Stage 3: PAPPL prototype on port 8001

```sh
cd ~/konica206-native
export KONICA_RASTER_FILTER=...   # as determined in 1.2
./konica206-native server -o server-port=8001 -o log-level=debug 2> /tmp/konica206-native.log &
sleep 2
./konica206-native add -d konica206 -m konica-bizhub-206 \
    -v 'konica://KONICA%20MINOLTA/206?serial=A8A6041029423'
./konica206-native printers
```
Notes:
- The printer is added with `-d NAME` (not as a positional argument).
- State is stored in `/var/lib/konica206-native.state` when run as root, otherwise in the user's home. If
  Stage 3 is rerun and behaves oddly, stop the server and delete that state file (it is this app's own file).
- Logs go to stderr (`-o log-file=PATH` to redirect). Use `-o log-level=debug` while testing.

Verify the IPP attributes (no paper used):
```sh
ipptool -tv ipp://localhost:8001/ipp/print/konica206 get-printer-attributes.test 2>&1 | \
  grep -E 'printer-make-and-model|media-default|sides|printer-resolution|document-format-supported|print-color-mode'
```
Expected:
- `printer-make-and-model = KONICA MINOLTA 206`
- `media-default = iso_a4_210x297mm`, `sides-default = one-sided`
- `sides-supported` = one-sided, two-sided-long-edge, two-sided-short-edge
- `printer-resolution-default = 600dpi`, supported 300 and 600
- `print-color-mode-supported = monochrome`
- formats include `application/pdf`, `image/pwg-raster`, `image/urf`, `image/jpeg`, `image/png`

If attributes like `printer-make-and-model` or `media-supported` are **missing**, PAPPL rejected the driver data
silently; rerun with `-o log-level=debug` and read the `Invalid driver ...` lines.

No-hardware end-to-end test (writes job output to a directory instead of USB):
```sh
mkdir -p /tmp/konica-out
./konica206-native add -d konica206-file -m konica-bizhub-206 -v 'konicafile:///tmp/konica-out'
ipptool -tv -f known-good.pdf -d filetype=application/pdf \
   ipp://localhost:8001/ipp/print/konica206-file print-job.test
ls -l /tmp/konica-out
```
Remove the test printer afterwards: `./konica206-native delete -d konica206-file`.

**STOP 3:** report the attribute check and the file-device job result.

---

## 6. Stage 4: physical tests (uses paper; confirm with the user before each batch)

Send via IPP to `ipp://localhost:8001/ipp/print/konica206` (copies, sides and media are IPP job attributes;
the `submit -n` shortcut of the built-in CLI does not forward copies, so use `ipptool` with an `.test` file
that sets `copies`/`sides`/`media`, or a temporary CUPS IPP queue created by the user).

Run each test independently, one at a time, and record pass/fail with the log lines:

1. One-page PDF, simplex, A4.
2. Multi-page PDF, simplex (page count correct, nothing missing).
3. `sides=two-sided-long-edge` (correct number of sheets, back side orientation correct).
4. `sides=two-sided-short-edge`.
5. A4 and Letter (`media=iso_a4_210x297mm`, `media=na_letter_8.5x11in`).
6. Tray selection (`media-col` source `tray-1` / `by-pass-tray`). **Known gap:** tray names are not yet mapped to PPD
   `InputSlot` values. Inspect `grep -E '^\*InputSlot' "$KONICA_PPD"` and report the real names to the user
   before changing code.
7. Copies = 2 or 3 (collated sets).
8. A PNG/JPEG image job (exercises the raster path; see "Known unknowns").
9. Unplug/replug the printer during a multi-page job: the app must not hang; the job should abort and the queue recover.

Check the logs after each test: `Render failed`, `USB transfer failed`, or `Unable to open device` lines.

### Known unknowns to resolve (do not guess; test and report)
- **Raster compatibility:** the PWG/URF/JPEG/PNG path hands the vendor filter an 8-bit gray, 600 dpi, compressed
  CUPS raster v2 stream. Confirm the printed result matches the PDF path. If the vendor filter rejects it, capture the
  raster header that `pdftoraster` produces for the same PPD and compare (`cupsrasterdump`-style) before changing code.
- **PPD option names:** `Resolution=...`, `PageSize` values other than A4/Letter and `InputSlot` are guesses.
  Compare against `grep -E '^\*(PageSize|InputSlot|Resolution|Duplex)' "$KONICA_PPD"`.
- **Status polling:** PAPPL opens/closes the USB device while idle. Watch for the printer misbehaving or dropping
  off the bus when idle for several minutes; report with `dmesg | tail`.
- **PostScript** is not advertised (PAPPL 1.3 has no PS path).

**STOP 4:** report the full pass/fail table. Do not proceed to Stage 5 unless every test the user cares about passes.

---

## 7. Stage 5: optional, only with the user's explicit approval

- Run as a service: copy `dist/konica206-native.service` to `/etc/systemd/system/`, adjust the `Environment=` lines,
  `sudo install -m755 konica206-native /usr/local/bin/`, then `systemctl daemon-reload && systemctl enable --now konica206-native`.
  Keep the port at 8001 until the old app is retired.
- GUI applications may need a CUPS queue pointing at the native endpoint (a new,
  separate queue; do not touch `KONICA_MINOLTA_206` or `konica206uri`).
  Attach the shipped PPD rather than `-m everywhere`:
  ```sh
  sudo lpadmin -p konica206-native -E \
    -v ipp://localhost:8001/ipp/print/konica206 \
    -P dist/konica206-native.ppd
  sudo lpadmin -p konica206-native \
    -o media-default=iso_a4_210x297mm -o sides-default=one-sided
  ```
- Why not `-m everywhere`: the native driver advertises full-bleed geometry
  (zero margins, matching the vendor filter's expectation), so CUPS'
  auto-generated `everywhere` PPD lists every size with a `.Borderless`
  suffix (`A4.Borderless`, `Letter.Borderless`, ...). Functionally identical,
  but confusing in print dialogs. `dist/konica206-native.ppd` was generated
  with `driverless ipp://localhost:8001/ipp/print/konica206` and carries real
  size names (`A4`, `Letter`, ...) with full-page image areas. The IPP values
  sent to the printer are the same either way, so the render path is unaffected.
- If the driver's media/option set ever changes, regenerate the file:
  ```sh
  driverless ipp://localhost:8001/ipp/print/konica206 > dist/konica206-native.ppd
  ```
  then re-attach it with `lpadmin -p konica206-native -P dist/konica206-native.ppd`.
- Compare output side by side with `konica206uri` before suggesting a switch.

---

## 8. Rollback

Everything installed by this guide is separate from the old setup, so rollback is:
```sh
./konica206-native shutdown            # or stop the systemd unit
sudo systemctl disable --now konica206-native 2>/dev/null
sudo rm -f /etc/systemd/system/konica206-native.service /usr/local/bin/konica206-native
sudo rm -f /etc/udev/rules.d/70-konica206.rules && sudo udevadm control --reload
lpadmin -x konica206-native 2>/dev/null     # only the queue this guide created
rm -f /var/lib/konica206-native.state
```
Then restart whatever old service you stopped in Stage 2 and confirm the old queue still prints.

---

## 9. Environment variables

| Variable | Default | Meaning |
|---|---|---|
| `KONICA_VENDOR_FILTER` | `/usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf` | Konica raster filter |
| `KONICA_RASTER_FILTER` | `/usr/lib/cups/filter/pdftoraster` | PDF -> CUPS raster |
| `KONICA_LIBDIR` | `/usr/local/lib/konica/lib` | Private libcups/libcupsimage |
| `KONICA_PPD` | `/var/lib/legacy-printer-app/ppd/KonicaMinolta-206-fullbleed.ppd` | PPD the filters read |
| `KONICA_SERIAL` | `A8A6041029423` | Restrict to this printer |
| `KONICA_SPOOLDIR` | `/var/tmp` | Per-job temp files |
| `KONICA_FILTER_TIMEOUT` | `300` | Seconds allowed per filter stage |

## 10. What to report back when finished

- Distro, PAPPL/libusb/CUPS versions, resolved filter paths.
- Result of each STOP checkpoint, including log excerpts for failures.
- Anything that differed from this guide (paths, package names, option names).
- Whether the old stack was stopped and whether it was restored.
