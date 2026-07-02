// license:GPLv3+

#pragma once

#include <filesystem>
#include <format>

#include <string>
using namespace std::string_literals;
using namespace std::string_view_literals;
using std::string;

#include "plugins/LoggingPlugin.h"

#include "plugins/VPXPlugin.h"

#ifdef _MSC_VER
#define PATH_SEPARATOR_CHAR '\\'
#else
#define PATH_SEPARATOR_CHAR '/'
#endif

namespace Vni {

// retroeki: this fork's LoggingPlugin.h ships only the printf-style LPI_*
// helpers (no _CPP std::string variants the upstream VNI plugin expects). Wrap
// each call so the plugin's std::string / std::format log arguments forward
// through "%s". Commas inside std::format(...) stay grouped by its parens.
LPI_USE();
#define LOGD(s) LPI_LOGD("%s", std::string(s).c_str())
#define LOGI(s) LPI_LOGI("%s", std::string(s).c_str())
#define LOGW(s) LPI_LOGW("%s", std::string(s).c_str())
#define LOGE(s) LPI_LOGE("%s", std::string(s).c_str())

void SetThreadName(const std::string& name);
std::filesystem::path find_case_insensitive_file_path(const std::filesystem::path& searchedFile);

// copies all characters of src incl. the null-terminator, BUT never more than dest_size-1, always null-terminates
inline void strncpy_s(char* const __restrict dest, const size_t dest_size, const char* const __restrict src)
{
   if (!dest || dest_size == 0)
      return;
   size_t i = 0;
   if (src)
      for (; i < dest_size-1 && src[i] != '\0'; ++i)
         dest[i] = src[i];
   dest[i] = '\0';
}

}
