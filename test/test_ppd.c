/* Regression test for the PPD helpers in src/konica_filter.c.
 * The live PPD uses the translated keyword form ("*PaperDimension A4/A4: ..."),
 * which ppd_paper_points() used to miss (every size silently fell back to
 * hardcoded A4 geometry). Run from the repo root:  make check-ppd
 * No printer, no USB, no paper. */
#include <stdio.h>
#include <string.h>

#include "../src/konica_filter.c"

static int fails = 0;

#define CHECK(cond, ...) \
  do { \
    if (!(cond)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } \
  } while (0)

int
main(int argc, char *argv[])
{
  const char *ppd = argc > 1 ? argv[1] : "test/ppd-fixture.ppd";
  double w = 0, h = 0;

  /* translated form: the case that used to miss */
  CHECK(ppd_paper_points(ppd, "A4", &w, &h) == 0 && w == 595 && h == 842,
        "A4 -> %g %g", w, h);
  CHECK(ppd_paper_points(ppd, "Letter", &w, &h) == 0 && w == 612 && h == 792,
        "Letter -> %g %g", w, h);
  CHECK(ppd_paper_points(ppd, "A3", &w, &h) == 0 && w == 842 && h == 1191,
        "A3 -> %g %g", w, h);
  CHECK(ppd_paper_points(ppd, "B5", &w, &h) == 0 && w == 516 && h == 729,
        "B5 -> %g %g", w, h);
  /* untranslated form keeps working */
  CHECK(ppd_paper_points(ppd, "Plain", &w, &h) == 0 && w == 100 && h == 200,
        "Plain -> %g %g", w, h);
  /* unknown size and partial-name prefix must miss, not match a sibling */
  CHECK(ppd_paper_points(ppd, "Statement", &w, &h) != 0, "Statement should miss");
  CHECK(ppd_paper_points(ppd, "A", &w, &h) != 0, "'A' must not match A3/A4");
  CHECK(ppd_paper_points("/nonexistent.ppd", "A4", &w, &h) != 0, "missing file");
  CHECK(ppd_paper_points(ppd, NULL, &w, &h) != 0, "NULL name");

  /* neighbours use prefix matching already; pin their behaviour too */
  CHECK(ppd_has_option(ppd, "PageSize", "A4") == 1, "has PageSize A4");
  CHECK(ppd_has_option(ppd, "PageSize", "Nope") == 0, "no PageSize Nope");
  CHECK(ppd_has_option(ppd, "Resolution", "600x600dpi") == 1, "has 600dpi");
  CHECK(ppd_media_position(ppd, "Tray1") == 3, "Tray1 -> %d", ppd_media_position(ppd, "Tray1"));
  CHECK(ppd_media_position(ppd, "Bypass") == 2, "Bypass -> %d", ppd_media_position(ppd, "Bypass"));
  CHECK(ppd_media_position(ppd, "Auto") == 1, "Auto -> %d", ppd_media_position(ppd, "Auto"));
  CHECK(ppd_media_position(ppd, "Tray9") == 0, "unknown slot -> 0");
  CHECK(ppd_backside(ppd) == 'R', "backside -> '%c'", ppd_backside(ppd));

  if (fails == 0)
    printf("ALL PPD TESTS PASSED (%s)\n", ppd);
  else
    printf("%d FAILURES (%s)\n", fails, ppd);
  return fails != 0;
}
