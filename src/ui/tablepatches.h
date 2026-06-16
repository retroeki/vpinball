// license:GPLv3+
#pragma once

#include <string>
#include <vector>
#include <utility>
#include <cstddef>

namespace tablepatches {

// One per-table script edit, scoped to a single VPX file (matched by basename,
// case-insensitive). `find`/`replace` are literal substrings unless `isRegex`,
// in which case `find` is a std::regex (ECMAScript) pattern applied with
// std::regex_replace. Keyed per file so even a regex entry affects only that file.
struct TablePatch {
   const char* filename;     // VPX basename, e.g. "DragonFire.vpx"
   const char* find;
   const char* replace;
   bool        isRegex;
   const char* description;  // for the patch log
};

// Production registry (defined in tablepatches.cpp).
extern const TablePatch kTablePatches[];
extern const size_t kTablePatchCount;

// Apply `patches[0..count)` to `script` for the table identified by `tableFilename`
// (matched on basename, case-insensitive). Returns the input byte-for-byte unchanged
// if nothing matches. When `applied` is non-null, each applied edit pushes
// {description, occurrences}.
std::string ApplyTableSpecificPatches(const std::string& script, const std::string& tableFilename,
   const TablePatch* patches, size_t count,
   std::vector<std::pair<std::string, int>>* applied = nullptr);

// Convenience overload using the production registry.
std::string ApplyTableSpecificPatches(const std::string& script, const std::string& tableFilename,
   std::vector<std::pair<std::string, int>>* applied = nullptr);

} // namespace tablepatches
