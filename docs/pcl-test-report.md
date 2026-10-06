# bizhub 206 Generic PCL5/PCL6 Test Report

Date: 2026-10-05 (IST)
Host: CUPS 2.4.16, cups-filters 2.0.1, Ghostscript 10.06.0
Procedure: `/home/spot/Downloads/bizhub-206-generic-pcl5-pcl6-test.md`
Result: CUPS-level PASS, physical print FAIL for all PCL paths. GDI control PASSES.

## 1. Baseline (untouched)

Existing queues before test:

```text
CUPS-PDF -> pdf-writer:/export/share/pdf/
EPSON_L3260 -> ipp://192.168.1.19:631/ipp/print
konica206uri -> ipp://localhost:8000/ipp/print/konica206uri
konica206uri-ppd -> ipp://localhost:8000/ipp/print/konica206uri
KONICA_MINOLTA_206 -> usb://KONICA%20MINOLTA/206?serial=A8A6041029423&interface=1
system default: KONICA_MINOLTA_206
```

USB:

```text
Bus 002 Device 003: ID 132b:232b KONICA MINOLTA KONICA MINOLTA 206
```

CUPS USB backend discovery Device ID:

```text
MFG:KONICA MINOLTA;CMD:GDI,XPS;MDL:206;CLS:PRINTER;CID:KONICA MINOLTA206;
direct usb://KONICA%20MINOLTA/206?serial=A8A6041029423&interface=1
```

Working GDI PPD `file_path:/etc/cups/ppd/KONICA_MINOLTA_206.ppd:16`:

```text
*ModelName: "KONICA MINOLTA 206 (ICC-free, full-bleed retrofit)"
*PCFileName: "245igdi.PPD"
*1284DeviceID: "MFG:KONICA MINOLTA;CMD:GDI,XPS;MDL:206;PRINTER"
*cupsFilter: "application/vnd.cups-raster 0 /usr/local/lib/konica/KonicaMinolta/245igdi/Filters/245igdirf"
```

Key: `CMD:GDI,XPS` — no PCL, no PCLXL, no PJL advertised.

## 2. Generic driver discovery

Expected in procedure:

```text
drv:///sample.drv/genericpcl5.ppd
drv:///sample.drv/genericpcl6.ppd
```

Do not exist on this system. `lpinfo -m` has only:

```text
drv:///sample.drv/generpcl.ppd Generic PCL Laser Printer
```

Closest Generic matches used:

```text
foomatic-db-compressed-ppds:0/ppd/foomatic-ppd/Generic-PCL_5e_Printer-ljet4.ppd
  Generic PCL 5e Printer Foomatic/ljet4
foomatic-db-compressed-ppds:0/ppd/foomatic-ppd/Generic-PCL_6_PCL_XL_Printer-pxlmono.ppd
  Generic PCL 6/PCL XL Printer Foomatic/pxlmono
```

Ghostscript devices available: `ljet4`, `pxlmono`, `pxlcolor` confirmed via `gs -h`.
CUPS filters available: `foomatic-rip`, `gstopxl`, `rastertopclx`, `rastertohp`, etc.

## 3. Temporary test queues (existing queues not modified)

Same USB URI as working queue, separate names:

```bash
USB_URI='usb://KONICA%20MINOLTA/206?serial=A8A6041029423&interface=1'
sudo lpadmin -p KM206-PCL5-TEST -v "$USB_URI" \
  -m foomatic-db-compressed-ppds:0/ppd/foomatic-ppd/Generic-PCL_5e_Printer-ljet4.ppd -E
sudo lpadmin -p KM206-PCL6-TEST -v "$USB_URI" \
  -m foomatic-db-compressed-ppds:0/ppd/foomatic-ppd/Generic-PCL_6_PCL_XL_Printer-pxlmono.ppd -E
```

Verified:

```text
KM206-PCL5-TEST -> usb://KONICA%20MINOLTA/206?serial=A8A6041029423&interface=1
KM206-PCL6-TEST -> usb://KONICA%20MINOLTA/206?serial=A8A6041029423&interface=1
system default destination: KONICA_MINOLTA_206 (unchanged)
```

PPD filter lines:

```text
KM206-PCL5-TEST: *NickName: "Generic PCL 5e Printer Foomatic/ljet4"
  *cupsFilter: "application/vnd.cups-pdf 0 foomatic-rip"
KM206-PCL6-TEST: *NickName: "Generic PCL 6/PCL XL Printer Foomatic/pxlmono"
```

## 4. Generic PCL tests

All with explicit `-d`, default never changed.

### 4.1 PCL5 text — Job 46 PASS (CUPS)

```bash
printf 'Konica Minolta bizhub 206\nGeneric PCL5 test\n\nIf this page prints correctly, PCL5 is working.\n' > ~/km206-pcl5-test.txt
lp -d KM206-PCL5-TEST ~/km206-pcl5-test.txt
# request id is KM206-PCL5-TEST-46
# Job 46 Job completed, printer idle, backend usb no errors
```

### 4.2 PCL5 PDF — Jobs 47/48/51 PASS (CUPS)

```bash
lp -d KM206-PCL5-TEST /usr/share/cups/data/standard.pdf
# Job 47 Job completed, Sent 7744 bytes, gs -sDEVICE=ljet4, backend usb no errors
# Job 48 duplicate via lpr, same 7744 bytes, completed
lp -d KM206-PCL5-TEST /usr/share/cups/data/default-testpage.pdf
# Job 51 Job completed, Sent 236833 bytes, backend usb no errors
```

Filter chain from `error_log`:

```text
texttopdf -> pdftopdf -> foomatic-rip (gs -sDEVICE=ljet4 -r600x600) -> usb
JCL: ESC%-12345X@PJL
renderer exited status 0
```

### 4.3 PCL6 text — Job 49 PASS (CUPS)

```bash
lp -d KM206-PCL6-TEST ~/km206-pcl6-test.txt
# Job 49 Job completed, Sent 10833 bytes
# gs -sDEVICE=pxlmono -r600x600
```

### 4.4 PCL6 PDF — Job 50 PASS (CUPS)

```bash
lp -d KM206-PCL6-TEST /usr/share/cups/data/standard.pdf
# Job 50 Job completed, Sent 1445 bytes, pxlmono, backend usb no errors
```

Physical result for 4.1–4.4: no paper out, confirmed by user.

## 5. Raw PCL bypass (eliminates foomatic packaging as cause)

Generated locally, valid headers verified with `od`:

```bash
gs ... -sDEVICE=ljet4 ... -sOutputFile=/tmp/direct-pcl5.pcl /usr/share/cups/data/standard.pdf
# 7739 bytes, starts 1b 45 1b 26 6c... (ESC E PCL)
gs ... -sDEVICE=pxlmono ... -sOutputFile=/tmp/direct-pcl6.pxl /usr/share/cups/data/standard.pdf
# 1445 bytes, starts 1b 25 2d 31 32 33 34 35 58 40 50 4a 4c... (@PJL ENTER LANGUAGE=PCLXL)
lp -o raw -d KM206-PCL5-TEST /tmp/direct-pcl5.pcl  # Job 52 completed
lp -o raw -d KM206-PCL6-TEST /tmp/direct-pcl6.pxl  # Job 53 completed
```

Both `page_log total 1`, `already completed`, backend no errors. No paper out.

## 6. Konica own PCL driver test

Package: `/root/Downloads/BH226PCL6Linux_015MU.zip`

```text
BH226PCL6Linux_015MU/Readme/English/Readme.txt:
  KONICA MINOLTA bizhub 206/226/246 Linux Printer Driver Ver. 1.0.0
BH226PCL6Linux_015MU/Redhat5/konica-minolta-246pcl-cups-0.16-1.x86_64.rpm
  usr/share/cups/model/KonicaMinolta/206pcl.ppd
  usr/share/cups/model/KonicaMinolta/226pcl.ppd
  usr/share/cups/model/KonicaMinolta/246pcl.ppd
  usr/lib64/cups/filter/KonicaMinolta/246pcl/Filters/246pclrf (ELF 64-bit, 2015)
  usr/lib64/cups/filter/KonicaMinolta/246pcl/Filters/mtorf.ocm
```

`206pcl.ppd`:

```text
*ModelName: "KONICA MINOLTA 206 PCL"
*1284DeviceID: "MFG:KONICA MINOLTA;CMD:PJL,PCL,PCLXL,XPS;MDL:206 PCL;CLS:PRINTER"
*cupsFilter: "application/vnd.cups-raster 0 /usr/lib/cups/filter/KonicaMinolta/246pcl/Filters/246pclrf"
```

Installed to:

```text
/usr/lib/cups/filter/KonicaMinolta/246pcl/Filters/246pclrf
/usr/lib/cups/filter/KonicaMinolta/246pcl/Profiles/sRGB.icm, sGray.icm
/usr/share/cups/model/KonicaMinolta/206pcl.ppd
```

Test queue:

```bash
sudo lpadmin -p KM206-KONICA-PCL-TEST -v "$USB_URI" \
  -P /usr/share/cups/model/KonicaMinolta/206pcl.ppd -E
# PageSize default *A4, options OCM_* same family as GDI
```

Tests:

```bash
lp -d KM206-KONICA-PCL-TEST ~/km206-pcl5-test.txt
# Job 54 Job completed, Sent 531823 bytes
# pdftopdf -> gstoraster -> 246pclrf -> usb, PAGE: 1 1, no errors
lp -d KM206-KONICA-PCL-TEST /usr/share/cups/data/standard.pdf
# Job 55 Job completed, Sent 517742 bytes, same chain, no errors
```

Physical result: no paper out, confirmed by user.

Mismatch: PPD expects `CMD:PJL,PCL,PCLXL,XPS`, device reports `CMD:GDI,XPS`.

## 7. GDI control — Job 56 PASS (physical)

```bash
lp -d KONICA_MINOLTA_206 /usr/share/cups/data/standard.pdf
# request id is KONICA_MINOLTA_206-56
# page_log: KONICA_MINOLTA_206 root 56 ... total 1
# printer idle, queue clear
```

User: "got a print". Printer hardware, USB, paper path OK via GDI `245igdirf`.

## 8. Results table

| Test | CUPS | Physical |
|---|---|---|
| Generic PCL5 text Job 46 | completed | FAIL |
| Generic PCL5 PDF Jobs 47/48 (7744B) | completed, ljet4 | FAIL |
| Generic PCL5 PDF Job 51 default-testpage (236833B) | completed | FAIL |
| Generic PCL6 text Job 49 (10833B) | completed, pxlmono | FAIL |
| Generic PCL6 PDF Job 50 (1445B) | completed | FAIL |
| Raw PCL5e Job 52 (7739B) | completed | FAIL |
| Raw PCL XL Job 53 (1445B) | completed | FAIL |
| Konica 206 PCL text Job 54 (531823B) | completed, 246pclrf | FAIL |
| Konica 206 PCL PDF Job 55 (517742B) | completed | FAIL |
| GDI Job 56 | completed, 245igdirf | PASS |
| Correct paper (A4 vs Letter) | PCL queues default Letter, Konica PCL default A4 | N/A — no PCL output to judge |
| Default printer preserved | KONICA_MINOLTA_206 throughout | PASS |

CUPS `Job completed` here means USB bulk transfer succeeded, not printer interpreted the PDL. Printer silently discards non-GDI data.

## 9. Interpretation

Per procedure §14 Both fail:

- Not a CUPS packaging bug: three independent PCL producers (foomatic ljet4, pxlmono, Konica 246pclrf) + raw passthrough all transfer cleanly but produce no output, while GDI on same USB URI prints.
- Device is GDI/host-based in current config. Base bizhub 206 print requires optional PCL controller/NIC (IC-206 class). No PCL menu/controller evident.
- Generic PCL path cannot replace legacy GDI/`cups-filters 1.28.x` dependency for this unit.

If PCL is required: check panel for Emulation/PCL option, optional IC/controller installed, firmware mode, or network-attached variant with different Device ID. Retest only if Device ID shows `PCL`/`PCLXL`.

## 10. Cleanup (§16 — only temp queues)

```bash
sudo lpadmin -x KM206-PCL5-TEST
sudo lpadmin -x KM206-PCL6-TEST
sudo lpadmin -x KM206-KONICA-PCL-TEST
lpstat -p
# CUPS-PDF, EPSON_L3260, konica206uri, konica206uri-ppd, KONICA_MINOLTA_206 remain
lpstat -d
# system default destination: KONICA_MINOLTA_206
```

Original Konica queue untouched: no `-x`, no PPD/URI change, no default change. Installed Konica PCL files left under `/usr/lib/cups/filter/KonicaMinolta/246pcl/` and `/usr/share/cups/model/KonicaMinolta/206pcl.ppd` (inert without queue).

## 11. Evidence locations

- `/var/log/cups/error_log`: Jobs 46–55 filter/backend lines, `foomatic-rip exited no errors`, `backend/usb exited no errors`
- `/var/log/cups/page_log`: Jobs 46–56 `total 1`
- `/tmp/direct-pcl5.pcl` (7739B), `/tmp/direct-pcl6.pxl` (1445B)
- `~/km206-pcl5-test.txt`, `~/km206-pcl6-test.txt`
- `/tmp/kmpcl6/extract/...` RPM contents

## Conclusion

bizhub 206 on `usb://KONICA%20MINOLTA/206?serial=A8A6041029423&interface=1` does not accept standard PCL5/PCL6 in its present GDI configuration, including via Konica's own `206 PCL` Linux driver. Keep GDI `245igdi` path.
