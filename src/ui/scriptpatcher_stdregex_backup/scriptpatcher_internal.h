#pragma once

/**
 * @file scriptpatcher_internal.h
 * @brief Internal utilities shared across scriptpatcher implementation files
 *
 * This header is for internal use only. External code should use scriptpatcher.h
 */

#include <string>
#include <algorithm>
#include <regex>
#include <cctype>

#ifdef __STANDALONE__

namespace scriptpatcher {
namespace internal {

/**
 * Trim whitespace from both ends of a string
 */
inline std::string Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

/**
 * Case-insensitive string comparison
 */
inline bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

/**
 * Convert string to lowercase
 */
inline std::string ToLower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

/**
 * Escape special regex characters in a string
 */
inline std::string EscapeRegex(const std::string& str) {
    static const std::regex specialChars(R"([-[\]{}()*+?.,\\^$|#\s])");
    return std::regex_replace(str, specialChars, "\\$&");
}

} // namespace internal
} // namespace scriptpatcher

// Convenience aliases for internal use
using scriptpatcher::internal::Trim;
using scriptpatcher::internal::EqualsIgnoreCase;
using scriptpatcher::internal::ToLower;
using scriptpatcher::internal::EscapeRegex;

#endif // __STANDALONE__
