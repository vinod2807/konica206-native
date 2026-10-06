# Plan: Native CUPS 3-Compatible PAPPL Application for Konica Minolta bizhub 206

## Objective

Create a standalone native PAPPL Printer Application for the USB-connected
Konica Minolta bizhub 206.

The application should expose a modern IPP printer and continue using the
known-working Konica GDI rendering path, without requiring CUPS classic PPD
queues, the CUPS USB backend, or `legacy-printer-app` at runtime.

## Current known-good components

Printer:

```text
USB VID:PID 132b:232b
Model: KONICA MINOLTA 206
Serial: A8A6041029423
USB interface: 1
Device ID: MFG:KONICA MINOLTA;CMD:GDI,XPS;MDL:206;PRINTER
```

Vendor renderer:

```text
/usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf
```

Vendor renderer dependencies are privately bundled:

```text
/usr/local/lib/konica/lib/libcups.so.2
/usr/local/lib/konica/lib/libcupsimage.so.2
```

Existing USB sender:

```text
/usr/local/libexec/konica-backend/usb
```

The USB sender is self-contained apart from `libusb-1.0` and libc. It sends
data in 8192-byte chunks because larger writes can disconnect the printer.

Existing successful pipeline:

```text
PDF/IPP job
  -> PAPPL/cups-filters raster processing
  -> Konica 245igdirf
  -> chunked libusb sender
  -> bizhub 206
```

## Proposed architecture

```text
Client application
        |
        | IPP 2.0
        v
Native Konica PAPPL application
        |
        +-- IPP job validation and option handling
        +-- PDF/PWG-raster input handling
        +-- media and duplex mapping
        +-- vendor raster filter invocation
        +-- private libcups/libcupsimage loading
        +-- direct libusb transfer
        v
Konica Minolta bizhub 206
```

The application should advertise an IPP endpoint such as:

```text
ipp://localhost:8001/ipp/print/konica206
```

Use a separate port during development. Do not replace the existing
`konica206uri` application until testing is complete.

## Design choice: wrapper versus full reimplementation

### Recommended first implementation: native PAPPL wrapper

Use PAPPL directly, but retain the proven closed-source `245igdirf` binary as
the raster renderer.

The native application would replace `legacy-printer-app` as the IPP server,
not replace the Konica renderer immediately.

Advantages:

- Lowest risk.
- Reuses the renderer that already produces working pages and duplex output.
- Retains the existing full-page geometry workaround.
- Avoids CUPS 3 classic PPD/filter/backend requirements at the CUPS boundary.
- Allows direct testing on an alternate port and printer name.

### Not recommended initially: pure renderer rewrite

Reimplementing `245igdirf` would require reverse-engineering Konica's private
GDI/PJL/raster protocol and image compression behavior. This is unnecessary
for the CUPS 3 migration and should be considered only if the vendor binary
becomes unusable.

## Main implementation pieces

### 1. PAPPL server

Implement a small C/C++ application using PAPPL APIs:

- application/server initialization;
- one printer device representing the bizhub 206;
- IPP printer attributes;
- job submission and cancellation;
- printer status and USB-disconnected state;
- optional web administration page;
- optional Avahi/DNS-SD advertisement.

The application should support at least:

```text
application/pdf
image/pwg-raster
image/urf
image/jpeg
image/png
application/postscript
```

PDF should be the preferred input format.

### 2. Driver/job callback

For every IPP job:

1. Validate `media`, `media-col`, `sides`, `copies`, resolution, tray, and
   vendor options.
2. Convert or receive the job as CUPS/PWG raster in a controlled temporary
   directory.
3. Invoke `245igdirf` with the correct raster input and options.
4. Capture the vendor filter's output.
5. Send the output through the custom libusb transfer code.
6. Report success only after the complete USB transfer succeeds.

The renderer must run with an explicit private library path or embedded RPATH
so it never depends on the host's CUPS ABI.

### 3. Direct USB transfer

Port the existing custom sender into the application or link it as a small
internal module.

Required behavior:

- Match VID `0x132b`, PID `0x232b`, and serial `A8A6041029423`.
- Use USB interface 1.
- Locate the bulk OUT endpoint dynamically.
- Detach/claim the interface safely.
- Write no more than 8192 bytes per transfer.
- Use a bounded transfer timeout.
- Return clear retry/failure status if the printer disappears.
- Avoid relying on `/usr/lib/cups/backend/usb`.

The existing `/root/konica-usb-backend.c` is a reference implementation.

### 4. Rendering integration

There are two possible approaches:

#### Option A: use PAPPL/libcupsfilters raster facilities

Preferred if the installed PAPPL APIs provide a stable raster callback.
Generate a standard PWG/CUPS raster stream and pass it to `245igdirf`.

#### Option B: invoke a private helper process

Use a small bundled helper that:

- receives PDF or PWG raster;
- invokes Ghostscript/libcupsfilters privately;
- invokes `245igdirf`;
- writes final printer data to stdout or a pipe.

Option B is easier for the first prototype but should have strict argument
handling, temporary-file cleanup, timeouts, and exit-status checks.

Do not call system CUPS filters by path unless they are intentionally bundled
with the application. The goal is to avoid a dependency on host CUPS 2.x
behavior.

## IPP attributes to advertise

The current PAPPL endpoint already provides a useful baseline. The native app
should advertise:

```text
printer-make-and-model = KONICA MINOLTA 206
document-format-preferred = application/pdf
document-format-supported = application/pdf, image/pwg-raster, image/urf
media-supported = A3, A4, A5, A6, B4, B5, B6, Letter, Legal, etc.
sides-supported = one-sided, two-sided-long-edge, two-sided-short-edge
printer-resolution-supported = 300x300dpi, 600x600dpi
print-color-mode-supported = monochrome
```

The default should be:

```text
media-default = iso_a4_210x297mm
sides-default = one-sided
printer-resolution-default = 600x600dpi
```

Do not copy the current stale PAPPL state default of Letter. Set and validate
the default in the new application's own IPP attributes.

## Duplex handling

Duplex must be driven from the IPP `sides` attribute:

```text
one-sided                  -> Duplex=false
two-sided-long-edge        -> Duplex=true, Tumble=false
two-sided-short-edge       -> Duplex=true, Tumble=true
```

The existing successful configuration also uses rotated backside handling and
full-page image geometry. Preserve the normalized full-page geometry from:

```text
/var/lib/legacy-printer-app/ppd/KonicaMinolta-206-fullbleed.ppd
```

Do not use the old real-margin PPD geometry without testing; it previously
caused ghosting/misalignment in the PAPPL path.

## CUPS 3 compatibility requirements

The new application must not require the CUPS scheduler to:

- load a PPD;
- run a CUPS filter;
- run a CUPS backend;
- link the vendor filter against host `libcups.so.2`;
- manage the USB device directly.

CUPS 3 should see only an IPP Printer Application.

The application itself may internally use PAPPL, libcupsfilters, libppd, or
private compatibility libraries as implementation details. Those dependencies
must be bundled or declared for the application, not required by cupsd.

## Development stages

### Stage 1 — offline renderer proof

- Build a small helper outside CUPS.
- Feed it a known PDF.
- Produce the same output size and structure as a known successful
  `legacy-printer-app` job.
- Verify `245igdirf` exit status and private library resolution.
- Do not connect the helper to the printer yet.

### Stage 2 — USB sender proof

- Send a previously captured known-good printer stream through the new direct
  libusb code.
- Use a separate test command, not a CUPS queue.
- Verify 8192-byte chunking and disconnect handling.

### Stage 3 — PAPPL prototype

- Start on port 8001.
- Advertise a test printer named `konica206-native`.
- Keep the existing `konica206uri` application running.
- Test `Get-Printer-Attributes` and IPP job submission without paper first.

### Stage 4 — physical test

Test independently:

1. one-page PDF simplex;
2. multi-page PDF simplex;
3. two-sided long-edge;
4. two-sided short-edge;
5. A4 and Letter;
6. tray selection;
7. multiple copies;
8. printer unplug/reconnect during a job.

### Stage 5 — migration decision

Only after all tests pass:

- compare native app output against `konica206uri`;
- verify applications can use the native IPP endpoint;
- create an optional CUPS IPP proxy queue if required by GUI applications;
- retain the current queues as rollback paths;
- never make the migration destructive.

## Acceptance criteria

The native application is ready only when:

- PDF prints correctly;
- duplex prints on the correct number of sheets;
- A4 is the default;
- no host CUPS PPD/filter/backend is needed;
- `ldd` shows the vendor filter using private bundled CUPS libraries;
- USB transfers remain stable with 8192-byte writes;
- printer removal does not hang the application;
- the application exposes valid IPP 2.0 attributes;
- CUPS 3 can use it through IPP;
- the existing queues remain available for rollback.

## Main risks

1. PAPPL API differences between installed and target distributions.
2. `245igdirf` assumptions about CUPS raster headers or filter environment.
3. Vendor filter failures for unusual media sizes.
4. USB interface ownership conflicts with the existing application.
5. Hidden state in the current PAPPL PPD or OCM resource tree.
6. Vendor filter licensing and redistribution restrictions.
7. Application-level differences in PDF/PostScript classification.

## Recommendation

Build the native PAPPL wrapper first, not a new GDI renderer. Develop it as a
parallel application on a separate port and printer name. This gives the
largest CUPS 3 compatibility improvement with the least risk and preserves
the only rendering path proven to work on this GDI-only bizhub 206.

No system files, queues, services, or packages should be changed until the
prototype passes the offline and isolated PAPPL tests.
