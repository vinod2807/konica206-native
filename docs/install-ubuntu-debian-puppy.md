# Implementing konica206-native on Ubuntu / Debian / Puppy

Target: Konica Minolta bizhub 206 (USB `132b:232b`, GDI-only) via the
native PAPPL app in this repo. End state: an IPP printer on
`localhost:8001` plus an optional CUPS proxy queue, with the existing
CUPS setup untouched.

> Companion: `INSTALL-konica206-native.md` (staged agent procedure with
> STOP gates). This file is the human schematic: what differs per distro,
> exact commands, and every gotcha met on real machines.

## 0. What you need before starting

- The printer on USB. Verify:
  ```sh
  lsusb | grep 132b:232b
  lsusb -v -d 132b:232b 2>/dev/null | grep -E 'bInterfaceNumber|bInterfaceClass'
  ```
  Expect interface 1 (printer class). The app talks to interface 1 only.
- The Konica vendor tree (closed GDI filter + private libs + full-bleed PPD):
  ```sh
  test -x /usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf && echo filter-ok
  LD_LIBRARY_PATH=/usr/local/lib/konica/lib ldd \
    /usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf | grep libcups
  test -r /var/lib/legacy-printer-app/ppd/KonicaMinolta-206-fullbleed.ppd && echo ppd-ok
  grep -n 'DefaultOCM_TonerSave' \
    /var/lib/legacy-printer-app/ppd/KonicaMinolta-206-fullbleed.ppd
  # TonerSave must default TRUE or pages print solid black
  ```
  If `ldd` resolves `libcups` from `/usr/lib…` instead of
  `/usr/local/lib/konica/lib`, stop: the private-library requirement
  is not met (see §6).
- A PDF → raster filter for the system renderer path:
  ```sh
  ls -l /usr/lib/cups/filter/pdftoraster /usr/libexec/cups/filter/pdftoraster 2>&1
  ```
  Export whichever exists as `KONICA_RASTER_FILTER`.
- Only one owner of USB interface 1 at a time. If a `legacy-printer-app`
  (`konica206uri`, port 8000) is running, plan who stops when (see §5).

## 1. Packages

### Ubuntu 24.04/26.04 and Debian 12/13

```sh
sudo apt update
sudo apt install -y build-essential pkg-config libpappl-dev \
  libusb-1.0-0-dev libcups2-dev cups-filters ghostscript \
  poppler-utils usbutils curl cups-ipp-utils
```

### Puppy Linux (ResolutePup64, EasyOS and similar)

Same list via the Puppy package manager where available; minimum set:
`gcc`, `make`, `pkgconf`, PAPPL dev files, `libusb-1.0` dev files,
CUPS dev files, `cups-filters`, Ghostscript, `poppler-utils`,
`usbutils`, `curl`. If a `-dev` package is missing, search first
(`apt-cache search pappl`) rather than guessing names.

### Gotchas seen on real installs

1. **No `pkg-config` binary.** Ubuntu 26.04 ships `pkgconf`
   (`/usr/bin/pkgconf`) while the `Makefile` calls `pkg-config`.
   Do NOT install anything extra — use a project-local link:
   ```sh
   cd konica206-native
   ln -s /usr/bin/pkgconf pkg-config
   PATH="$PWD:$PATH" make
   ```
   Never put this link in `/usr/bin`; it is a build-environment quirk,
   not a system fix.
2. **Interrupted `dpkg`.** If an install is ever killed mid-way, repair
   before anything else:
   ```sh
   sudo dpkg --configure -a
   dpkg --audit   # expect empty output
   ```
3. **`libdbus-1-dev` postinst failure.** If it fails on
   `update-xmlcatalog: command not found`, the `xml-core` package is in
   a half state: `sudo apt-get install --reinstall -y xml-core`, then
   `sudo dpkg --configure -a`.
4. **`cups-config` noise.** `dpkg-architecture: command not found` lines
   from `cups-config --version` are harmless on minimal/Puppy systems;
   the version it prints (2.4.x) is still valid.
5. **Remove the toolchain when done.** The standing practice on the shop
   machine is a compiler-free system. After building, keep runtime
   `libpappl1t64` (the old stack needs it) and remove the rest:
   ```sh
   sudo apt-get purge -y build-essential pkg-config \
     libpappl-dev libusb-1.0-0-dev libcups2-dev
   sudo apt-mark manual libpappl1t64
   sudo apt-get autoremove --purge -y
   dpkg --audit; apt-get check   # both must be clean
   ```

## 2. Build and offline check (no printer, no paper)

```sh
mkdir -p ~/konica206-native && cd ~/konica206-native
tar xzf /path/to/konica206-native.tar.gz --strip-components=1
make
make check          # fake-USB suite, must print ALL PASSED
```

Expected binaries: `konica206-native`, `konica-send`, `konica-render`,
`test-chunking`. Read compiler errors literally; typical causes are a
missing `-dev` package or PAPPL older than 1.3. Required versions:
PAPPL ≥ 1.3 (built against 1.4.9), CUPS 2.4 headers (CUPS 3 guarded but
uncompiled), libusb-1.0.

Renderer proof (offline):

```sh
export KONICA_RASTER_FILTER=/usr/lib/cups/filter/pdftoraster
./konica-render /usr/share/cups/data/standard.pdf /tmp/konica-test.bin
# expect: OK: wrote N bytes
```

## 3. USB test (one page, no CUPS, no PAPPL)

Stop anything holding USB interface 1 first (see §5), then:

```sh
./konica-send /tmp/konica-test.bin
# expect: opened serial=... out=0x.. in=0x.. / sent N bytes
```

USB rules that are load-bearing, not advisory:

- Never write more than **8192 bytes** per bulk transfer (larger writes
  drop the printer off the bus; observed at ~61 KB).
- Device URI form: `konica://KONICA%20MINOLTA/206?serial=<serial>`.
- Optional udev rule (needs sudo; harmless without it if testing as root):
  ```sh
  sudo cp dist/70-konica206.rules /etc/udev/rules.d/
  sudo udevadm control --reload && sudo udevadm trigger
  ```

## 4. Run the app (port 8001, separate from anything existing)

```sh
export KONICA_RASTER_FILTER=/usr/lib/cups/filter/pdftoraster
./konica206-native server -o server-port=8001 -o log-level=info &
./konica206-native add -d konica206 -m konica-bizhub-206 \
  -v 'konica://KONICA%20MINOLTA/206?serial=A8A6041029423'
./konica206-native printers
```

Notes:

- State file: `/var/lib/konica206-native.state` as root, else under the
  user’s home. If a rerun behaves oddly, stop the server and delete
  *only this app’s* state file.
- Logs go to stderr; redirect to a file with `-o log-file=PATH`.
- Environment reference (`KONICA_VENDOR_FILTER`, `KONICA_LIBDIR`,
  `KONICA_PPD`, `KONICA_SERIAL`, `KONICA_SPOOLDIR`,
  `KONICA_FILTER_TIMEOUT`, `KONICA_STUCK_SECS` default 600,
  `KONICA_LISTEN`, `KONICA_MAX_MEM_MB`/`KONICA_MAX_FILE_MB`,
  `KONICA_PDF_RENDERER=system|builtin`): see `README.md`.
- **Renderer default is `system`.** `builtin` is proven for simplex and,
  after the back-side rotation fix, long-edge duplex — but the live
  default stays `system` until its soak completes. Switch only with
  `KONICA_PDF_RENDERER=builtin` in the app’s environment.

Verify without paper:

```sh
ipptool -tv ipp://localhost:8001/ipp/print/konica206 \
  get-printer-attributes.test
```

Expect: make/model `KONICA MINOLTA 206`, `media-default
iso_a4_210x297mm`, `sides` one-sided + both two-sided, resolutions 300
and 600 (default 600), monochrome, formats pdf/pwg-raster/urf/jpeg/png.

## 5. Coexistence with the old stack

- Old: `legacy-printer-app` on port **8000** (`konica206uri`) and classic
  queue `KONICA_MINOLTA_206`. New: port **8001**, printer `konica206`.
- Both servers may be *installed* at once, but only one may *print* at a
  time (USB interface 1 is exclusive). Stagger all physical tests.
- Never modify, delete, or restart the old app/queues/driver files as
  part of this install. They are the rollback path.

## 6. CUPS proxy queue (optional, for GUI apps)

Use the shipped PPD, **not** `-m everywhere`:

```sh
sudo lpadmin -p konica206-native -E \
  -v ipp://localhost:8001/ipp/print/konica206 \
  -P dist/konica206-native.ppd
sudo lpadmin -p konica206-native \
  -o media-default=iso_a4_210x297mm -o sides-default=one-sided
```

Why: `-m everywhere` lists every size with a `.Borderless` suffix
(because the driver is full-bleed). The shipped PPD carries real names.
**Do not reintroduce `ISOB4`/`ISOB5` sizes**: those names are not
standard Windows paper IDs and crash Wine’s `gdi32` (divide-by-zero)
when printing from Adobe Reader/Notepad. After any queue regeneration,
check `lpoptions -p konica206-native -l | grep -i isob` is empty.

Never send CUPS banner/test pages through this queue
(`application/vnd.cups-pdf-banner` wedges the printer).

## 7. Autostart per distro

### Debian / Ubuntu with systemd

```sh
# adjust Environment= lines first
sudo cp dist/konica206-native.service /etc/systemd/system/
sudo install -m755 konica206-native /usr/local/bin/
sudo systemctl daemon-reload
# Do NOT enable until the old stack is retired (one USB owner):
# sudo systemctl enable --now konica206-native
```

Harden one directive at a time, printing after each; never set
`PrivateDevices=yes` (USB access required).

### Puppy (no systemd as PID 1)

Unit files are inert here (PID 1 is `busybox init`). Mirror the
existing legacy-app pattern instead — a boot script such as
`/root/Startup/konica206-native.sh` that starts the server if it is not
already running, same shape as `legacy-printer-app-konica.sh`. Keep the
systemd unit in the repo for Debian/Ubuntu machines.

## 8. Wine / Adobe Reader notes

- Wine mirrors CUPS PPDs into its prefix at print-dialog time; after
  any queue PPD change, fully close Adobe so it re-syncs.
- If Adobe shows 94% preview scaling or crashes at Print click with a
  `gdi32` divide-by-zero, suspect non-standard size names first.
- Wine headless printing (`notepad /p`, `printui /k`) is unreliable in
  this setup (hangs, unimplemented commands) — verify with real GUI
  prints, one sheet at a time.

## 9. Troubleshooting

| Symptom | Likely cause |
|---|---|
| `claim_interface (another process owns the printer?)` | old stack holds interface 1 — stop it first |
| `printer stopped accepting data` | power-cycle the printer |
| Job completes, nothing prints | wrong renderer stream or banner job; check app log for `Render failed` / `USB transfer failed` |
| `filter not executable` | wrong `KONICA_RASTER_FILTER`/`KONICA_VENDOR_FILTER` path |
| Vendor filter libcups errors | private `LD_LIBRARY_PATH` not effective; verify with `ldd` |
| CUPS queue stuck | server not running on 8001, or old app owns USB |
| `make` fails on `pkg-config` | use the project-local `pkgconf` link (§1) |
| `dpkg` in a mess | `sudo dpkg --configure -a`, then `dpkg --audit` |

## 10. Rollback (everything here is separate from the old setup)

```sh
./konica206-native shutdown            # or stop the systemd unit
sudo systemctl disable --now konica206-native 2>/dev/null
sudo rm -f /etc/systemd/system/konica206-native.service /usr/local/bin/konica206-native
sudo rm -f /etc/udev/rules.d/70-konica206.rules && sudo udevadm control --reload
lpadmin -x konica206-native 2>/dev/null     # only the queue created here
rm -f /var/lib/konica206-native.state
```

Then restart the old stack and confirm the old queue still prints.
