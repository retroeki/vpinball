// license:GPLv3+
//
// patch_corpus -- run the REAL SimpleScriptPatcher::PatchScript over a whole
// folder of extracted VBScripts, so the ENTIRE patcher pipeline (all enabled
// core rules + InjectHelpers + the per-table tablepatches layer + the
// CRLF/non-ASCII finalize pass) can be exercised, linted, and diffed at corpus
// scale. This is the "Layer 2" harness sketched in TESTING_PLAN.md, built
// against the vendored RE2 so it is byte-faithful to the on-device engine
// (no Python re-port to drift out of sync with the C++).
//
//   patch_corpus <in_dir> <out_dir> [report.csv]
//
//   in_dir  : directory of pre-patch *.vbs scripts (see ../extract_vpx_scripts.py)
//   out_dir : patched *.vbs written here, one per input (created if missing)
//   report  : optional CSV summary (table,changed,patches)
//
// The input file's stem is reused as the tableFilename ("<stem>.vpx") so the
// filename-keyed per-table patch layer fires exactly as it does on device.

#include "ui/simplescriptpatcher.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// SimpleScriptPatcher::DumpScript() calls this; returning nullptr makes the
// patcher skip its on-device debug dump. It is the ONLY app symbol the patcher
// references, so stubbing it here keeps the harness self-contained.
extern "C" const char* VPinballGetInternalPath() { return nullptr; }

static std::string ReadFile(const fs::path& p)
{
   std::ifstream f(p, std::ios::binary);
   std::ostringstream ss;
   ss << f.rdbuf();
   return ss.str();
}

static void WriteFile(const fs::path& p, const std::string& s)
{
   std::ofstream f(p, std::ios::binary);
   f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

// Collapse the multi-line patch report into one "; "-joined line.
static std::string OneLineReport(const std::string& r)
{
   std::string out;
   std::istringstream in(r);
   std::string line;
   while (std::getline(in, line)) {
      const size_t a = line.find_first_not_of(" \t\r\n");
      if (a == std::string::npos)
         continue;
      const size_t b = line.find_last_not_of(" \t\r\n");
      if (!out.empty())
         out += "; ";
      out += line.substr(a, b - a + 1);
   }
   return out;
}

// Minimal CSV field escaping.
static std::string Csv(const std::string& s)
{
   if (s.find_first_of(",\"\n") == std::string::npos)
      return s;
   std::string out = "\"";
   for (char c : s) {
      if (c == '"')
         out += '"';
      out += c;
   }
   out += "\"";
   return out;
}

int main(int argc, char** argv)
{
   if (argc < 3) {
      std::cerr << "usage: patch_corpus <in_dir> <out_dir> [report.csv]\n";
      return 2;
   }
   const fs::path inDir = argv[1];
   const fs::path outDir = argv[2];
   const fs::path reportPath = (argc >= 4) ? fs::path(argv[3]) : fs::path();

   if (!fs::is_directory(inDir)) {
      std::cerr << "input dir not found: " << inDir << '\n';
      return 2;
   }
   fs::create_directories(outDir);

   std::vector<fs::path> files;
   for (const auto& e : fs::directory_iterator(inDir))
      if (e.is_regular_file() && e.path().extension() == ".vbs")
         files.push_back(e.path());
   std::sort(files.begin(), files.end());

   std::ofstream csv;
   if (!reportPath.empty()) {
      csv.open(reportPath, std::ios::binary);
      csv << "table,changed,patches\n";
   }

   size_t total = 0, changed = 0, errors = 0;
   for (const auto& f : files) {
      ++total;
      const std::string stem = f.stem().string();
      const std::string script = ReadFile(f);
      std::string patched;
      try {
         patched = SimpleScriptPatcher::PatchScript(script, stem + ".vpx");
      } catch (const std::exception& ex) {
         ++errors;
         std::cout << "ERROR  " << stem << ": " << ex.what() << '\n';
         if (csv.is_open())
            csv << Csv(stem) << ",ERROR," << Csv(ex.what()) << '\n';
         continue;
      }
      WriteFile(outDir / (stem + ".vbs"), patched);

      const std::string report = OneLineReport(SimpleScriptPatcher::GetLastPatchReport());
      const bool didChange = !report.empty();
      if (didChange) {
         ++changed;
         std::cout << "PATCHED " << stem << ": " << report << '\n';
      }
      if (csv.is_open())
         csv << Csv(stem) << ',' << (didChange ? "1" : "0") << ',' << Csv(report) << '\n';
   }

   std::cout << "\n--- patch_corpus summary ---\n"
             << "tables    : " << total << '\n'
             << "changed   : " << changed << '\n'
             << "unchanged : " << (total - changed - errors) << '\n'
             << "errors    : " << errors << '\n'
             << "output    : " << outDir.string() << '\n';
   return 0;
}
