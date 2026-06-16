// license:GPLv3+
#include "ui/tablepatches.h"
#include <algorithm>
#include <cctype>
#include <regex>

namespace tablepatches {

const TablePatch kTablePatches[] = {
   { "DragonFire.vpx", "Desktop=Table1.ShowDT", "Desktop=True", false,
     "DragonFire: script Desktop=True so the LED score reels populate (engine ShowDT unchanged)" },
};
const size_t kTablePatchCount = sizeof(kTablePatches) / sizeof(kTablePatches[0]);

namespace {
std::string Basename(const std::string& path) {
   const size_t s = path.find_last_of("/\\");
   return (s == std::string::npos) ? path : path.substr(s + 1);
}
std::string ToLower(std::string s) {
   for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
   return s;
}
int ReplaceAllLiteral(std::string& s, const std::string& find, const std::string& repl) {
   if (find.empty()) return 0;
   int n = 0;
   size_t pos = 0;
   while ((pos = s.find(find, pos)) != std::string::npos) {
      s.replace(pos, find.size(), repl);
      pos += repl.size();
      ++n;
   }
   return n;
}
} // namespace

std::string ApplyTableSpecificPatches(const std::string& script, const std::string& tableFilename,
   const TablePatch* patches, size_t count,
   std::vector<std::pair<std::string, int>>* applied) {
   const std::string base = ToLower(Basename(tableFilename));
   if (base.empty())
      return script; // unknown table -> no-op
   std::string result = script;
   for (size_t i = 0; i < count; ++i) {
      const TablePatch& p = patches[i];
      if (ToLower(p.filename) != base)
         continue;
      int n = 0;
      if (p.isRegex) {
         try {
            const std::regex re(p.find);
            std::string out = std::regex_replace(result, re, std::string(p.replace));
            if (out != result) { result = std::move(out); n = 1; }
         } catch (const std::regex_error&) {
            n = 0; // bad pattern -> skip safely
         }
      } else {
         n = ReplaceAllLiteral(result, p.find, p.replace);
      }
      if (n > 0 && applied)
         applied->push_back({ std::string(p.description), n });
   }
   return result;
}

std::string ApplyTableSpecificPatches(const std::string& script, const std::string& tableFilename,
   std::vector<std::pair<std::string, int>>* applied) {
   return ApplyTableSpecificPatches(script, tableFilename, kTablePatches, kTablePatchCount, applied);
}

} // namespace tablepatches
