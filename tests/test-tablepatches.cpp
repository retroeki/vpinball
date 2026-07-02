// license:GPLv3+
#include "ui/tablepatches.h"
#include <cstdio>
#include <string>
using namespace tablepatches;

static int g_fail = 0;
#define CHECK(cond, msg) do { if (cond) printf("PASS  %s\n", msg); else { printf("FAIL  %s\n", msg); ++g_fail; } } while (0)

int main() {
   // #4 target correctness (literal, production registry): every occurrence of the
   // gate is flipped (DragonFire's script has two), nothing else changes.
   {
      const std::string s = "  If Desktop=True Then\r\n  Foo=1\r\n  If Desktop=True Then\r\n";
      CHECK(ApplyTableSpecificPatches(s, "tables/DragonFire.vpx") == "  If True Then\r\n  Foo=1\r\n  If True Then\r\n",
            "DragonFire.vpx: both branch gates patched, nothing else changed");
   }
   // #3a filename-gated: same script under a different filename -> unchanged.
   {
      const std::string s = "  If Desktop=True Then\r\n";
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
      CHECK(ApplyTableSpecificPatches("If Desktop=True Then", "C:\\X\\DRAGONFIRE.VPX") == "If True Then",
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
      const std::string once = ApplyTableSpecificPatches("If Desktop=True Then", "DragonFire.vpx");
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
      ApplyTableSpecificPatches("If Desktop=True Then", "DragonFire.vpx", &log);
      CHECK(log.size() == 1 && log[0].second == 1, "applied: one entry, count 1 for one literal edit");
      std::vector<std::pair<std::string, int>> nolog;
      ApplyTableSpecificPatches("If Desktop=True Then", "SomethingElse.vpx", &nolog);
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
   // RoboCop (production registry): Wall15 friction zeroed at the top of Table1_Init,
   // nothing else touched.
   {
      const std::string s = "Sub Table1_Init\r\n  vpmInit Me\r\nEnd Sub\r\n";
      CHECK(ApplyTableSpecificPatches(s, "Robocop (Data East 1989)_Bigus(MOD)3.0.vpx")
               == "Sub Table1_Init\r\n    Wall15.Friction = 0\r\n  vpmInit Me\r\nEnd Sub\r\n",
            "RoboCop.vpx: Wall15.Friction=0 injected at Table1_Init, rest unchanged");
      CHECK(ApplyTableSpecificPatches(s, "SomeOther.vpx") == s,
            "RoboCop patch: no-op on a different table");
   }
   // 4 Queens (production registry): the `If ShowDT=false Then` hide-DTItems gate is forced
   // true so the native reels never render in-scene; filename-gated, nothing else touched.
   {
      const std::string s = "\tIf ShowDT=false Then\r\n\t\tobj.visible=False\r\n\tend If\r\n";
      CHECK(ApplyTableSpecificPatches(s, "4 Queens (Bally 1970).vpx")
               == "\tIf True Then\r\n\t\tobj.visible=False\r\n\tend If\r\n",
            "4 Queens.vpx: ShowDT=false gate flipped to True, rest unchanged");
      CHECK(ApplyTableSpecificPatches(s, "SomeOther.vpx") == s,
            "4 Queens patch: no-op on a different table");
   }
   // Beat Time / Circus (production registry): the score-reel ResetToZero is hoisted ahead of the
   // `If Table1.ShowDT = True` gate (ungated) so the reels zero each new game; the gated block
   // (visibility swaps) is left intact. Regex entries, filename-gated, one per table.
   {
      const std::string s =
         "\t\tIf Table1.ShowDT = True then\r\n"
         "\t\t\tFor each obj in PlayerScores\r\n"
         "\t\t\t\tobj.ResetToZero\r\n"
         "\t\t\t\tobj.Visible=true\r\n"
         "\t\t\tnext\r\n";
      const std::string bt = ApplyTableSpecificPatches(s, "Beat Time (Williams 1967).vpx");
      CHECK(bt != s && bt.find("ResetToZero") < bt.find("If Table1.ShowDT = True then")
               && bt.find("obj.Visible=true") != std::string::npos,
            "Beat Time.vpx: ResetToZero hoisted ahead of the ShowDT gate, visibility intact");
      const std::string cr = ApplyTableSpecificPatches(s, "Circus (Zaccaria 1977)_Teisen_2.2_burger.vpx");
      CHECK(cr != s && cr.find("ResetToZero") < cr.find("If Table1.ShowDT = True then"),
            "Circus.vpx: ResetToZero hoisted ahead of the ShowDT gate");
      CHECK(ApplyTableSpecificPatches(s, "SomeOther.vpx") == s,
            "reel-reset hoist: no-op on an unregistered table");
   }
   printf("%s (%d failures)\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
   return g_fail ? 1 : 0;
}
