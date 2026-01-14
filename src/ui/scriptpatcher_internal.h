#pragma once

/**
 * @file scriptpatcher_internal.h
 * @brief Internal utilities shared across scriptpatcher implementation files
 *
 * This header is for internal use only. External code should use scriptpatcher.h
 *
 * Uses Google RE2 for regex operations (much faster than std::regex)
 */

#include <string>
#include <algorithm>
#include <cctype>
#include <memory>
#include <re2/re2.h>

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
 * Escape special regex characters in a string for RE2
 */
inline std::string EscapeRegex(const std::string& str) {
    std::string escaped;
    escaped.reserve(str.size() * 2);
    for (char c : str) {
        switch (c) {
            case '\\': case '.': case '+': case '*': case '?':
            case '(': case ')': case '[': case ']': case '{': case '}':
            case '^': case '$': case '|':
                escaped += '\\';
                break;
        }
        escaped += c;
    }
    return escaped;
}

/**
 * Create RE2 options for case-insensitive matching
 */
inline RE2::Options MakeRE2Options(bool case_insensitive = false, bool multiline = false) {
    RE2::Options opts;
    opts.set_case_sensitive(!case_insensitive);
    // RE2 treats ^ and $ as matching at newlines when dot_nl is false
    // and one_line is false (default behavior matches multiline)
    if (multiline) {
        opts.set_one_line(false);
    }
    opts.set_log_errors(false);  // Don't spam stderr
    return opts;
}

/**
 * Helper class for RE2 pattern with options (replaces std::regex)
 */
class RE2Pattern {
public:
    RE2Pattern(const std::string& pattern, bool case_insensitive = false, bool multiline = false)
        : re_(pattern, MakeRE2Options(case_insensitive, multiline)) {}

    bool ok() const { return re_.ok(); }
    const RE2& re() const { return re_; }
    const std::string& pattern() const { return re_.pattern(); }
    const std::string& error() const { return re_.error(); }

private:
    RE2 re_;
};

/**
 * Check if pattern matches anywhere in text (like std::regex_search)
 */
inline bool RE2Search(const std::string& text, const RE2& pattern) {
    return RE2::PartialMatch(text, pattern);
}

/**
 * Check if pattern matches anywhere in text with captures
 * Returns true if match found, and fills in captured groups
 */
inline bool RE2SearchWithCaptures(const std::string& text, const RE2& pattern,
                                   std::string* cap1 = nullptr,
                                   std::string* cap2 = nullptr,
                                   std::string* cap3 = nullptr,
                                   std::string* cap4 = nullptr,
                                   std::string* cap5 = nullptr) {
    if (cap1 == nullptr) {
        return RE2::PartialMatch(text, pattern);
    } else if (cap2 == nullptr) {
        return RE2::PartialMatch(text, pattern, cap1);
    } else if (cap3 == nullptr) {
        return RE2::PartialMatch(text, pattern, cap1, cap2);
    } else if (cap4 == nullptr) {
        return RE2::PartialMatch(text, pattern, cap1, cap2, cap3);
    } else if (cap5 == nullptr) {
        return RE2::PartialMatch(text, pattern, cap1, cap2, cap3, cap4);
    } else {
        return RE2::PartialMatch(text, pattern, cap1, cap2, cap3, cap4, cap5);
    }
}

/**
 * Global replace (like std::regex_replace)
 */
inline std::string RE2Replace(const std::string& text, const RE2& pattern, const std::string& rewrite) {
    std::string result = text;
    RE2::GlobalReplace(&result, pattern, rewrite);
    return result;
}

/**
 * Structure to hold match information (similar to std::smatch)
 */
struct RE2Match {
    std::string full_match;      // The entire matched substring
    std::vector<std::string> groups;  // Captured groups (group 0 = full match)
    size_t position = 0;         // Position of match in original string
    size_t length = 0;           // Length of full match

    std::string operator[](size_t idx) const {
        if (idx == 0) return full_match;
        if (idx <= groups.size()) return groups[idx - 1];
        return "";
    }

    size_t size() const { return groups.size() + 1; }
};

/**
 * Find all matches in text (similar to std::sregex_iterator)
 * Returns vector of RE2Match objects
 */
inline std::vector<RE2Match> RE2FindAll(const std::string& text, const RE2& pattern) {
    std::vector<RE2Match> matches;
    re2::StringPiece input(text);

    int num_groups = pattern.NumberOfCapturingGroups();
    std::vector<re2::StringPiece> captures(num_groups + 1);  // +1 for full match

    size_t offset = 0;
    while (pattern.Match(input, offset, input.size(), RE2::UNANCHORED,
                         captures.data(), captures.size())) {
        RE2Match m;
        m.full_match = std::string(captures[0].data(), captures[0].size());
        m.position = captures[0].data() - text.data();
        m.length = captures[0].size();

        for (int i = 1; i <= num_groups; i++) {
            m.groups.push_back(std::string(captures[i].data(), captures[i].size()));
        }

        matches.push_back(m);

        // Advance past this match
        offset = m.position + std::max<size_t>(1, m.length);
        if (offset >= text.size()) break;
    }

    return matches;
}

/**
 * Find first match in text with position information
 * Returns true if match found
 */
inline bool RE2FindFirst(const std::string& text, const RE2& pattern, RE2Match& match,
                          size_t start_pos = 0) {
    re2::StringPiece input(text);

    int num_groups = pattern.NumberOfCapturingGroups();
    std::vector<re2::StringPiece> captures(num_groups + 1);

    if (!pattern.Match(input, start_pos, input.size(), RE2::UNANCHORED,
                       captures.data(), captures.size())) {
        return false;
    }

    match.full_match = std::string(captures[0].data(), captures[0].size());
    match.position = captures[0].data() - text.data();
    match.length = captures[0].size();
    match.groups.clear();

    for (int i = 1; i <= num_groups; i++) {
        if (captures[i].data()) {
            match.groups.push_back(std::string(captures[i].data(), captures[i].size()));
        } else {
            match.groups.push_back("");
        }
    }

    return true;
}

/**
 * Iterator-style find all matches (for compatibility with existing code patterns)
 * This allows incremental iteration over matches
 */
class RE2MatchIterator {
public:
    RE2MatchIterator(const std::string& text, const RE2& pattern)
        : text_(text), pattern_(pattern), offset_(0), done_(false) {
        num_groups_ = pattern_.NumberOfCapturingGroups();
        captures_.resize(num_groups_ + 1);
        advance();
    }

    bool done() const { return done_; }

    const RE2Match& current() const { return current_; }

    void next() {
        if (!done_) {
            offset_ = current_.position + std::max<size_t>(1, current_.length);
            advance();
        }
    }

private:
    void advance() {
        if (offset_ >= text_.size()) {
            done_ = true;
            return;
        }

        re2::StringPiece input(text_);
        if (!pattern_.Match(input, offset_, input.size(), RE2::UNANCHORED,
                           captures_.data(), captures_.size())) {
            done_ = true;
            return;
        }

        current_.full_match = std::string(captures_[0].data(), captures_[0].size());
        current_.position = captures_[0].data() - text_.data();
        current_.length = captures_[0].size();
        current_.groups.clear();

        for (int i = 1; i <= num_groups_; i++) {
            if (captures_[i].data()) {
                current_.groups.push_back(std::string(captures_[i].data(), captures_[i].size()));
            } else {
                current_.groups.push_back("");
            }
        }
    }

    const std::string& text_;
    const RE2& pattern_;
    size_t offset_;
    bool done_;
    int num_groups_;
    std::vector<re2::StringPiece> captures_;
    RE2Match current_;
};

/**
 * Custom replacement function that allows a callback for each match
 * Similar to JavaScript's replace with function argument
 */
template<typename Callback>
std::string RE2ReplaceWithCallback(const std::string& text, const RE2& pattern, Callback callback) {
    std::string result;
    result.reserve(text.size());

    re2::StringPiece input(text);
    int num_groups = pattern.NumberOfCapturingGroups();
    std::vector<re2::StringPiece> captures(num_groups + 1);

    size_t last_end = 0;
    size_t offset = 0;

    while (offset < text.size() &&
           pattern.Match(input, offset, input.size(), RE2::UNANCHORED,
                        captures.data(), captures.size())) {
        size_t match_start = captures[0].data() - text.data();
        size_t match_end = match_start + captures[0].size();

        // Append text before this match
        result.append(text, last_end, match_start - last_end);

        // Build RE2Match for callback
        RE2Match m;
        m.full_match = std::string(captures[0].data(), captures[0].size());
        m.position = match_start;
        m.length = captures[0].size();
        for (int i = 1; i <= num_groups; i++) {
            if (captures[i].data()) {
                m.groups.push_back(std::string(captures[i].data(), captures[i].size()));
            } else {
                m.groups.push_back("");
            }
        }

        // Get replacement from callback
        result.append(callback(m));

        last_end = match_end;
        offset = match_end;
        if (captures[0].size() == 0) offset++;  // Prevent infinite loop on zero-width match
    }

    // Append remaining text
    result.append(text, last_end, text.size() - last_end);

    return result;
}

} // namespace internal
} // namespace scriptpatcher

// Convenience aliases for internal use
using scriptpatcher::internal::Trim;
using scriptpatcher::internal::EqualsIgnoreCase;
using scriptpatcher::internal::ToLower;
using scriptpatcher::internal::EscapeRegex;
using scriptpatcher::internal::RE2Pattern;
using scriptpatcher::internal::RE2Search;
using scriptpatcher::internal::RE2SearchWithCaptures;
using scriptpatcher::internal::RE2Replace;
using scriptpatcher::internal::RE2Match;
using scriptpatcher::internal::RE2FindAll;
using scriptpatcher::internal::RE2FindFirst;
using scriptpatcher::internal::RE2MatchIterator;
using scriptpatcher::internal::RE2ReplaceWithCallback;
using scriptpatcher::internal::MakeRE2Options;

#endif // __STANDALONE__
