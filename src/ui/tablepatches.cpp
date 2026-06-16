// license:GPLv3+
#include "ui/tablepatches.h"
#include <algorithm>
#include <cctype>
#include <iterator>
#include <regex>

namespace tablepatches {

const TablePatch kTablePatches[] = {
   // Route the LED score/text to the Led DispReels (b2sParseString picks the REEL glyph
   // table; b2sSendToBG writes leddisp.SetValue) WITHOUT flipping the global `Desktop`
   // flag. Both routines gate on `If Desktop=True Then` (that exact text appears only in
   // those two subs). Forcing the global Desktop=True instead would also skip the init
   // `If Desktop=false` block that hides the reels, leaving 40 backglass reels visible
   // (camera auto-fit glitch). Leaving Desktop=False keeps them hidden; ReelDmd reads
   // reel values regardless of visibility, so the in-app DMD panel still renders the score.
   { "DragonFire.vpx", "If Desktop=True Then", "If True Then", false,
     "DragonFire: route LED display to the (hidden) Led reels without un-hiding them" },
};
const size_t kTablePatchCount = sizeof(kTablePatches) / sizeof(kTablePatches[0]);

namespace {
std::string Basename(const std::string& path) {
   size_t end = path.size();
   while (end > 0 && (path[end - 1] == '/' || path[end - 1] == '\\'))
      --end; // ignore trailing separators so "foo/bar/" -> "bar"
   const size_t s = path.find_last_of("/\\", end == 0 ? std::string::npos : end - 1);
   const size_t start = (s == std::string::npos) ? 0 : s + 1;
   return path.substr(start, end - start);
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
            const int matchCount = static_cast<int>(
               std::distance(std::sregex_iterator(result.cbegin(), result.cend(), re), std::sregex_iterator()));
            if (matchCount > 0) {
               result = std::regex_replace(result, re, std::string(p.replace));
               n = matchCount; // accurate count for the `applied` contract {description, occurrences}
            }
         } catch (const std::regex_error&) {
            // A malformed pattern in the (author-controlled) kTablePatches registry is a
            // safe no-op; the entry's own unit test is expected to catch a bad pattern.
            n = 0;
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
