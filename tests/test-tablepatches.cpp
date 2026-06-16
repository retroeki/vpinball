// license:GPLv3+
#include "ui/tablepatches.h"
#include <cstdio>
#include <string>
using namespace tablepatches;

static int g_fail = 0;
#define CHECK(cond, msg) do { if (cond) printf("PASS  %s\n", msg); else { printf("FAIL  %s\n", msg); ++g_fail; } } while (0)

int main() {
   // #4 target correctness (literal, production registry): only the find->replace changes.
   {
      const std::string s = "  Desktop=Table1.ShowDT\r\n  Foo=1\r\n";
      CHECK(ApplyTableSpecificPatches(s, "tables/DragonFire.vpx") == "  Desktop=True\r\n  Foo=1\r\n",
            "DragonFire.vpx: Desktop flip applied, nothing else changed");
   }
   // #3a filename-gated: same script under a different filename -> unchanged.
   {
      const std::string s = "  Desktop=Table1.ShowDT\r\n";
      CHECK(ApplyTableSpecificPatches(s, "tables/SomethingElse.vpx") == s,
            "non-DragonFire filename: byte-identical no-op");
   }
   // #3b registered filename but the find text is absent -> unchanged.
   {
      const std::string s = "  NoMatchHere=1\r\n";
      CHECK(ApplyTableSpecificPatches(s, "DragonFire.vpx") == s,
            "DragonFire.vpx, find absent: unchanged");
   }
   // case-insensitive + path-stripped basename match.
   {
      CHECK(ApplyTableSpecificPatches("Desktop=Table1.ShowDT", "C:\\X\\DRAGONFIRE.VPX") == "Desktop=True",
            "case-insensitive, path-stripped match");
   }
   // #2 isolation across other scripts / empty filename -> byte-identical.
   {
      const std::string s = "Dim x\r\nx = Table1.ShowDT\r\n' unrelated\r\n";
      CHECK(ApplyTableSpecificPatches(s, "ElDorado.vpx") == s, "ElDorado.vpx: no-op");
      CHECK(ApplyTableSpecificPatches(s, "") == s, "empty filename: no-op");
   }
   // #6 idempotent: second apply is a no-op (find text gone).
   {
      const std::string once = ApplyTableSpecificPatches("Desktop=Table1.ShowDT", "DragonFire.vpx");
      CHECK(once == ApplyTableSpecificPatches(once, "DragonFire.vpx"), "idempotent");
   }
   // #5 regex mode (custom test registry): applies on keyed file, no-op elsewhere.
   {
      const TablePatch testPatches[] = {
         { "RegexTable.vpx", "Foo\\s*=\\s*[0-9]+", "Foo=99", true, "test regex" },
      };
      const std::string s = "Foo  =   7\r\n";
      CHECK(ApplyTableSpecificPatches(s, "RegexTable.vpx", testPatches, 1) == "Foo=99\r\n",
            "regex entry applies on keyed file");
      CHECK(ApplyTableSpecificPatches(s, "Other.vpx", testPatches, 1) == s,
            "regex entry no-op on other file");
   }
   // applied out-parameter: one entry per applied edit; empty when no match (production registry).
   {
      std::vector<std::pair<std::string, int>> log;
      ApplyTableSpecificPatches("Desktop=Table1.ShowDT", "DragonFire.vpx", &log);
      CHECK(log.size() == 1 && log[0].second == 1, "applied: one entry, count 1 for one literal edit");
      std::vector<std::pair<std::string, int>> nolog;
      ApplyTableSpecificPatches("Desktop=Table1.ShowDT", "SomethingElse.vpx", &nolog);
      CHECK(nolog.empty(), "applied: empty when no match");
   }
   // regex applied count reflects actual occurrences (custom registry).
   {
      const TablePatch testPatches[] = {
         { "Multi.vpx", "X", "Y", true, "regex multi" },
      };
      std::vector<std::pair<std::string, int>> log;
      const std::string out = ApplyTableSpecificPatches("X X X", "Multi.vpx", testPatches, 1, &log);
      CHECK(out == "Y Y Y", "regex multi: all occurrences replaced");
      CHECK(log.size() == 1 && log[0].second == 3, "applied: regex count == 3 occurrences");
   }
   printf("%s (%d failures)\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
   return g_fail ? 1 : 0;
}
