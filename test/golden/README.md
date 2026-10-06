# Golden outputs (Phase 0 baseline, 2026-10-06)

Rendered offline with `konica-render` (system `pdftoraster` + `245igdirf`,
private libs, full-bleed PPD, A4, `DefaultOCM_TonerSave=TRUE`).

| Document | Output | Size | SHA-256 |
|---|---|---|---|
| `/usr/share/cups/data/standard.pdf` | `standard-a4.bin` | 3811 | `1437057b…42c3ffd9` |
| `/usr/share/cups/data/default-testpage.pdf` | `default-testpage-a4.bin` | 63869 | `b3621109…ce2c4006` |

Determinism check: each document rendered twice; both runs byte-identical
(`cmp` clean), so no timestamps/job-ids leak into the vendor stream for
these inputs. Full hashes in `SHA256SUMS`.
