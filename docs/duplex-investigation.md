# Duplex binding investigation: builtin vs system renderer

Date: 2026-10-06. App: `konica206-native` (native PAPPL, bizhub 206 GDI).
Question: why do `builtin`-rendered duplex jobs bind short-edge while
`system`-rendered ones bind long-edge?

## Method

Same document (`ACCE03…-II_PUC.pdf`, 3 pages A4 portrait, scanned), same
queue (`konica206-native`), same options (`PageSize=A4 InputSlot=Tray1
Duplex=DuplexNoTumble sides=two-sided-long-edge 600dpi`), pages 1–2
(one duplex sheet). Power-cycle before every trial. Vendor input raster
and output stream captured per trial (`KONICA_CAPTURE_DIR`).

Golden control: 2-page duplex of CUPS `standard.pdf`, same options.

## Results

| Test | Renderer | Power-cycle | Binding |
|---|---|---|---|
| builtin void | builtin | no | short |
| system T2 | system | yes | **long** |
| builtin T3 | builtin | yes | short |
| system T4 | system | yes | **long** |
| golden std2, builtin | builtin | yes | short |

Deterministic across power-cycles: renderer-correlated, not flakiness.

## Measurements (all equivalent except pixels)

- Raster headers, every field, both pages: `4961x7016, Duplex=1,
  Tumble=0, MP=3, PS=595x842, BBox=0,0,595,842, NCopy=1` — identical.
- PJL control strings: identical except `IMAGELEN` byte counts
  (compressed-size bookkeeping).
- Stream tails (`PAGESTATUS=END`, `EOJ`, `UEL`): identical.
- Decoded final bitmaps: 99.4% identical bits both pages.
- Pixel diffs live in one band only (text antialiasing); no shift,
  rotation, flip, or placement difference found (tested explicitly).

Per-page diff on the failing document: p1 ~9%, p2 ~39% (watermark/
annotation rendering differences between Ghostscript and poppler,
including ~2.4% fully-inverted pixels) — but orientation-independent.

## Fixes applied along the way (kept)

1. `MediaPosition` was 0 on builtin vs 3 on system → vendor emitted
   `MEDIASOURCE=AUTO` instead of `TRAY1`. Fixed: header now mapped from
   PPD `InputSlot` (`commit: Builtin: set raster MediaPosition`).
2. `ImagingBoundingBox` was `0,0,0,0` and borderless scale `0.0` on
   builtin vs `0,0,595,842` / `1.0` on system. Fixed to mirror.
3. Neither fix changed the binding outcome.

## Conclusion

Content-deterministic (Case 1 per procedure) but **no control-byte cause
identified**: headers, PJL, tails and placement are equivalent, yet the
printer binds the streams differently. No binding/tumble/header patch
was made. Live default stays `system` (pinned in code, service file and
docs); `builtin` remains behind `KONICA_PDF_RENDERER=builtin`, safe for
single-sided work. The full evidence package (captures, hashes,
golden vendor streams) lives in `/root/konica206-duplex-investigation/`
(machine-local; raster captures too large for git).
