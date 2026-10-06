# konica206-native

Standalone PAPPL Printer Application for the Konica Minolta bizhub 206 (USB, GDI-only).
Implements Stages 1-3 of `konica206-native-pappl-plan.md`.

```
IPP client -> PAPPL -> pdftoraster -> 245igdirf (private libcups) -> libusb, 8192-byte chunks -> bizhub 206
                 \-> PWG/URF/JPEG/PNG -> CUPS raster -> 245igdirf -> libusb
```

No cupsd queue, PPD, CUPS filter chain or CUPS USB backend is involved at runtime.

## Build
    sudo dnf install pappl-devel libusb1-devel cups-devel gcc make     # Fedora
    make && make check

## Staged testing (matches the plan)
**Stage 1: renderer proof, no printer**
    ./konica-render known.pdf out.bin           # prints filters, PPD, libdir, options used
    ldd -r with LD_LIBRARY_PATH=/usr/local/lib/konica/lib \
        /usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf
Compare `out.bin` size/structure with a known-good `legacy-printer-app` capture.

**Stage 2: USB proof, no CUPS**
    sudo cp dist/70-konica206.rules /etc/udev/rules.d/ && sudo udevadm control --reload && sudo udevadm trigger
    ./konica-send out.bin                        # same sender code as the app

**Stage 3: PAPPL prototype on port 8001**
    ./konica206-native server -o server-port=8001 &
    ./konica206-native add -d konica206 -m konica-bizhub-206 \
        -v 'konica://KONICA%20MINOLTA/206?serial=A8A6041029423'
    ipptool -tv ipp://localhost:8001/ipp/print/konica206 get-printer-attributes.test

Stop the old `konica206uri` app (or unplug/replug) before a physical test: both claim USB interface 1.

## Configuration (environment variables)
| Variable | Default |
|---|---|
| KONICA_VENDOR_FILTER | /usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf |
| KONICA_RASTER_FILTER | /usr/lib/cups/filter/pdftoraster |
| KONICA_LIBDIR | /usr/local/lib/konica/lib (private libcups/libcupsimage) |
| KONICA_PPD | /var/lib/legacy-printer-app/ppd/KonicaMinolta-206-fullbleed.ppd |
| KONICA_SERIAL | A8A6041029423 |
| KONICA_SPOOLDIR | /var/tmp |
| KONICA_FILTER_TIMEOUT | 300 (seconds per stage) |
| KONICA_STUCK_SECS | 600 (0 disables the stuck-job watchdog) |
| KONICA_LISTEN | `localhost` (or `any`); server `-o listen=` overrides |
| KONICA_MAX_MEM_MB | 2048 (0 disables address-space cap on filter children) |
| KONICA_MAX_FILE_MB | 512 (0 disables output-size cap on filter children) |

## Unsupported inputs

`application/postscript` is deliberately unadvertised (PAPPL has no PS
render path in this driver). Behaviour, verified 2026-10-06:

- Direct IPP: job is aborted (`aborted-by-system`); the server log names
  the format (`Unable to process job with format 'application/postscript'`).
  PAPPL 1.4 offers no hook for a custom rejection message, so this terse
  abort is the explicit rejection — send PDF instead.
- Via the CUPS proxy queue: CUPS converts PS to PDF itself (`gstopdf`,
  verified offline) and the job prints normally.

## Test-only device
`konicafile:///some/dir` writes each job to `dir/job-*.bin` so the whole IPP path can be tested with no printer.

## What was verified (cloud sandbox, no printer, no vendor filter)
- PAPPL builds and serves IPP: A4 default, one-sided default, 300/600 dpi (600 default), monochrome,
  sides = one-sided / long / short edge, formats pdf, pwg-raster, urf, jpeg, png.
- libusb layer against a fake libusb: serial match, endpoint discovery on interface 1, no transfer over 8192 bytes,
  partial writes resumed, stall -> ETIMEDOUT after bounded retries, unplug -> ENODEV.
- Job path with stand-in filters: option mapping (PageSize/Duplex/Tumble/Resolution), private LD_LIBRARY_PATH,
  non-zero exit / timeout -> job aborted, nothing sent, temp files removed, copies replayed as collated streams.
- Raster path: PNG job produced a valid compressed CUPS raster v2 stream (8-bit gray, 600 dpi, Duplex set).

## NOT verified (needs your machine)
1. Real `245igdirf` output, and whether it accepts PAPPL's 8-bit gray raster the same way as pdftoraster's.
2. Real USB transfers and the printer's reaction to PAPPL's idle status polling (it opens/closes the device).
3. PPD option names: `Resolution=`, `InputSlot` (tray) and non-A4/Letter `PageSize` values are guesses; check
   `grep -E '^\*(PageSize|InputSlot|Resolution|Duplex)' $KONICA_PPD`. Tray selection is not mapped yet.
4. `application/postscript` is not advertised (PAPPL 1.3 has no PS path; add a PS->PDF step if needed).
5. Vendor filter redistribution licence if this is ever packaged.
