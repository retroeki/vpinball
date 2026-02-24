/**
 * @file scriptpatcher_classes_emit.cpp
 * @brief VBScript Class Emulation Code Generation
 *
 * Generates Wine-compatible VBScript code from parsed class definitions:
 * - ClassName_Create() factory functions
 * - ClassName_MethodName() transformed methods
 * - ClassName_Get/Let_AccessorName() property accessors
 * - TransformMethodBody: converts Me.X to this_("X"), etc.
 *
 * Uses Google RE2 for regex operations (much faster than std::regex)
 *
 * ============================================================================
 * WARNING: FRAGILE REGEX PATTERNS - MODIFY WITH EXTREME CARE!
 * ============================================================================
 * This code was migrated from std::regex to RE2. RE2 does NOT support:
 * - Negative lookahead (?!...)
 * - Positive lookahead (?=...)
 * - Backreferences (\1, \2, etc.)
 *
 * These patterns were rewritten to work without lookaheads, but this required
 * capturing trailing boundaries and restoring them in replacements. Some edge
 * cases may still exist.
 *
 * CRITICAL: The whitespace patterns are very sensitive:
 * - Using \s+ can match NEWLINES, causing patterns to incorrectly span lines
 *   (e.g., a no-arg method call followed by "If" on the next line gets merged)
 * - Using [ \t]+ only matches horizontal whitespace (spaces/tabs)
 * - Changing between these WILL break different tables!
 *
 * If you modify regex patterns here, test with MULTIPLE tables including:
 * - Game of Thrones (complex class emulation, DMDSettings)
 * - Lord of the Rings (different class patterns)
 * - Any table with single-line If...Then...Else statements
 * ============================================================================
 */

#include "stdafx.h"

#ifdef __STANDALONE__

#include "scriptpatcher.h"
#include "scriptpatcher_internal.h"
#include <sstream>

// ============================================================================
// CLASS EMULATION - Phase 2: Code Generation
// ============================================================================

std::string ScriptPatcher::TransformMethodBody(const std::string& body, const VBClassDefinition& classDef,
                                               const std::vector<std::string>& methodParams) {
    std::string result = body;

    // Build a set of method parameter names (case-insensitive) to skip during transformation
    std::unordered_set<std::string> paramNames;
    for (const auto& p : methodParams) {
        std::string lower = p;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        paramNames.insert(lower);
    }

    // Helper lambda to check if a name matches a method parameter
    auto isMethodParam = [&paramNames](const std::string& name) -> bool {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        return paramNames.count(lower) > 0;
    };

    // Build sets of accessor names for quick lookup
    std::unordered_set<std::string> accessorNamesLower;
    for (const auto& acc : classDef.accessors) {
        std::string lower = acc.name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        accessorNamesLower.insert(lower);
    }

    // Helper lambda to check if a position is inside a VBScript comment
    // VBScript comments start with ' and extend to end of line
    auto isInsideComment = [](const std::string& str, size_t pos) -> bool {
        // Find the start of the current line
        size_t lineStart = 0;
        for (size_t i = pos; i > 0; --i) {
            if (str[i - 1] == '\n' || str[i - 1] == '\r') {
                lineStart = i;
                break;
            }
        }
        // Check if there's a ' between line start and pos that's not inside a string
        int quoteCount = 0;
        for (size_t i = lineStart; i < pos; ++i) {
            if (str[i] == '"') {
                quoteCount++;
            } else if (str[i] == '\'' && quoteCount % 2 == 0) {
                // Found a comment character outside of a string
                return true;
            }
        }
        return false;
    };

    // Me.Accessor handling - MUST be done BEFORE generic Me.Property transform!
    // Transform Me.accessor = value -> ClassName_Let_accessor this_, value
    // Transform Me.accessor -> ClassName_Get_accessor(this_)
    for (const auto& accessor : classDef.accessors) {
        std::string escapedName = EscapeRegex(accessor.name);

        if (EqualsIgnoreCase(accessor.type, "Let") || EqualsIgnoreCase(accessor.type, "Set")) {
            // Me.accessor = value -> ClassName_Let_accessor this_, value
            // IMPORTANT: Use non-greedy capture and stop at Else/Then/colon/comment/newline
            // to handle: If ... Then Me.X = True Else Me.X = False
            RE2 meLetRegex("(?i)\\bMe\\." + escapedName + "\\s*=\\s*([^:\\r\\n]+?)([ \\t]+(?:Else|Then)\\b|[ \\t]*(?::|'|\\r|\\n|$))");
            result = RE2Replace(result, meLetRegex,
                classDef.name + "_" + accessor.type + "_" + accessor.name + " this_, \\1\\2");
        }

        if (EqualsIgnoreCase(accessor.type, "Get")) {
            // Me.accessor -> ClassName_Get_accessor(this_)
            // Must not be followed by = (that's handled by Let above)
            RE2 meGetRegex("(?i)\\bMe\\." + escapedName + "\\b");
            result = RE2ReplaceWithCallback(result, meGetRegex, [&](const RE2Match& m) -> std::string {
                // Check if followed by = (already handled by Let transform)
                size_t afterPos = m.position + m.length;
                while (afterPos < result.length() && (result[afterPos] == ' ' || result[afterPos] == '\t')) {
                    afterPos++;
                }
                if (afterPos < result.length() && result[afterPos] == '=') {
                    return m.full_match;  // Keep original - Let will handle it
                }
                return classDef.name + "_Get_" + accessor.name + "(this_)";
            });
        }
    }

    // Me.Property -> this_("Property") - ONLY for non-accessor properties
    // Accessors were already handled above
    static const RE2 meDotPattern(R"((?i)\bMe\.(\w+))");
    result = RE2ReplaceWithCallback(result, meDotPattern, [&](const RE2Match& m) -> std::string {
        std::string propName = m.groups.size() > 0 ? m.groups[0] : "";
        std::string lowerProp = propName;
        std::transform(lowerProp.begin(), lowerProp.end(), lowerProp.begin(), ::tolower);

        // Skip if this is an accessor (already transformed above)
        if (accessorNamesLower.count(lowerProp) > 0) {
            return m.full_match;  // Keep original - shouldn't happen since accessors handled above
        }
        return "this_(\"" + propName + "\")";
    });

    // Standalone Me -> this_
    // Note: Me.Property already handled above, so remaining Me won't be followed by dot
    static const RE2 mePattern(R"((?i)\bMe\b)");
    result = RE2Replace(result, mePattern, "this_");

    for (const auto& prop : classDef.properties) {
        std::string escapedName = EscapeRegex(prop.name);

        if (prop.isArray) {
            // Array properties: replace occurrences with global variable name
            // e.g., ballvel -> CoRTracker_ballvel (used as global)
            // Array properties are global (shared) because Dictionaries can't hold arrays
            std::string globalName = classDef.name + "_" + prop.name;

            // VPX object properties that should NOT be transformed when accessed on external objects
            // These exist on VPX visual objects (lamps, etc.) and aren't class-specific
            static const std::unordered_set<std::string> vpxProperties = {
                "fadespeedup", "fadespeeddown", "state", "timerenabled", "timerinterval",
                "x", "y", "width", "height", "name", "visible", "enabled", "image"
            };
            std::string lowerPropName = prop.name;
            std::transform(lowerPropName.begin(), lowerPropName.end(), lowerPropName.begin(), ::tolower);
            bool isVpxProperty = vpxProperties.count(lowerPropName) > 0;

            // Transform object.PropName -> global name
            // For VPX properties (like FadeSpeedUp), only transform Me./this_. prefix
            // For class-specific properties (like ModIn), transform any object prefix
            if (isVpxProperty) {
                // Only transform Me.PropName or this_.PropName (not arbitrary objects)
                RE2 mePropRegex("(?i)\\b(Me|this_)\\." + escapedName + "\\b");
                result = RE2Replace(result, mePropRegex, globalName);
            } else {
                // Transform any object.PropName (class-specific array, aObj must be same class)
                RE2 objPropRegex("(?i)\\b\\w+\\." + escapedName + "\\b");
                result = RE2Replace(result, objPropRegex, globalName);
            }

            // Then replace standalone PropName -> ClassName_PropName
            RE2 allPattern("(?i)\\b" + escapedName + "\\b");
            {
                std::string temp;
                auto matches = RE2FindAll(result, allPattern);
                size_t lastPos = 0;
                for (const auto& match : matches) {
                    size_t matchPos = match.position;
                    bool skip = false;
                    // Skip if preceded by dot (for VPX properties, this was left as aObj.PropName)
                    if (isVpxProperty && matchPos > 0 && result[matchPos - 1] == '.') {
                        skip = true;
                    }
                    // Skip if already transformed (preceded by ClassName_)
                    if (!skip && matchPos >= globalName.length()) {
                        std::string before = result.substr(matchPos - globalName.length(), globalName.length());
                        if (before == globalName.substr(0, globalName.length() - prop.name.length())) {
                            skip = true;  // Already has ClassName_ prefix
                        }
                    }
                    temp += result.substr(lastPos, matchPos - lastPos);
                    if (skip) {
                        temp += match.full_match;
                    } else {
                        temp += globalName;
                    }
                    lastPos = matchPos + match.length;
                }
                temp += result.substr(lastPos);
                if (!temp.empty()) result = temp;
            }
        } else {
            // Non-array properties: use Dictionary-based approach
            // First transform assignments: prop = value -> this_("prop") = value
            // Use iterative approach to skip matches preceded by a dot (e.g., aTrigger.Name = x)
            RE2 assignRegex("(?i)\\b" + escapedName + "(\\s*=\\s*)");
            {
                std::string assignTemp;
                auto matches = RE2FindAll(result, assignRegex);
                size_t aLastPos = 0;
                for (const auto& amatch : matches) {
                    size_t aMatchPos = amatch.position;
                    // Skip if this property name matches a method parameter
                    // e.g., Sub Setup(Name, ...) with property Name - don't transform parameter Name
                    bool aSkip = isMethodParam(prop.name);
                    // Check if preceded by dot
                    if (!aSkip) aSkip = (aMatchPos > 0 && result[aMatchPos - 1] == '.');
                    assignTemp += result.substr(aLastPos, aMatchPos - aLastPos);
                    if (aSkip) {
                        assignTemp += amatch.full_match;  // Keep original
                    } else {
                        assignTemp += "this_(\"" + prop.name + "\")" + (amatch.groups.size() > 0 ? amatch.groups[0] : "");
                    }
                    aLastPos = aMatchPos + amatch.length;
                }
                assignTemp += result.substr(aLastPos);
                if (!assignTemp.empty()) result = assignTemp;
            }

            // Then transform reads: prop -> this_("prop")
            // Skip matches already inside this_("...") or inside string literals
            RE2 readRegex("(?i)\\b" + escapedName + "\\b");

            std::string temp;
            auto matches = RE2FindAll(result, readRegex);
            size_t lastPos = 0;

            for (const auto& match : matches) {
                size_t matchPos = match.position;

                // Skip if this property name matches a method parameter
                // e.g., Sub Setup(Name, ...) with property Name - don't transform parameter Name
                bool skip = isMethodParam(prop.name);

                // Check if this is inside this_("...") - already transformed
                if (!skip && matchPos >= 7) {
                    std::string before = result.substr(matchPos - 7, 7);
                    if (before == "this_(\"") {
                        skip = true;
                    }
                }

                // Check if preceded by a dot - means it's accessing property on another object
                // e.g., aTrigger.Name should NOT become aTrigger.this_("Name")
                if (!skip && matchPos > 0) {
                    char charBefore = result[matchPos - 1];
                    if (charBefore == '.') {
                        skip = true;
                    }
                }

                // Check if inside a Dim statement - don't transform local variable declarations
                // e.g., "Dim a1, a2" should NOT become "Dim this_("a1"), this_("a2")"
                if (!skip && matchPos >= 4) {
                    // Find the start of the current statement (after last : or newline)
                    size_t stmtStart = 0;
                    for (size_t i = matchPos; i > 0; --i) {
                        char c = result[i - 1];
                        if (c == ':' || c == '\n' || c == '\r') {
                            stmtStart = i;
                            break;
                        }
                    }
                    std::string stmt = result.substr(stmtStart, matchPos - stmtStart);
                    // Check if statement starts with Dim (case-insensitive)
                    size_t firstNonSpace = stmt.find_first_not_of(" \t");
                    if (firstNonSpace != std::string::npos && firstNonSpace + 3 < stmt.length()) {
                        std::string keyword = stmt.substr(firstNonSpace, 4);
                        // Convert to lowercase for comparison
                        for (char& c : keyword) c = std::tolower(c);
                        if (keyword == "dim " || keyword == "dim\t") {
                            skip = true;  // Inside Dim statement
                        }
                    }
                }

                // Check if inside a string literal by counting quotes before this position
                if (!skip) {
                    int quoteCount = 0;
                    for (size_t i = 0; i < matchPos; ++i) {
                        if (result[i] == '"') quoteCount++;
                    }
                    // Odd number of quotes means we're inside a string
                    if (quoteCount % 2 == 1) {
                        skip = true;
                    }
                }

                // Check if inside a VBScript comment (after ' on the same line)
                if (!skip && isInsideComment(result, matchPos)) {
                    skip = true;
                }

                temp += result.substr(lastPos, matchPos - lastPos);
                if (skip) {
                    temp += match.full_match;  // Keep original
                } else {
                    temp += "this_(\"" + prop.name + "\")";
                }
                lastPos = matchPos + match.length;
            }
            temp += result.substr(lastPos);
            if (!temp.empty()) result = temp;
        }
    }

    // Transform internal method calls (calls to other methods of the same class without Me. prefix)
    // E.g., DisableState tmp(x) -> ClassName_DisableState this_, tmp(x)
    // E.g., TurnOnStates -> ClassName_TurnOnStates this_
    for (const auto& method : classDef.methods) {
        std::string escapedName = EscapeRegex(method.name);

        // Pattern for method call with arguments: methodName args
        // Match at statement boundaries (start of line, after :, after Then/Else)
        // Note: RE2 doesn't support (?!=) lookahead, so we handle this differently
        // IMPORTANT: Use [ \\t]+ not \\s+ - \\s includes newlines which would
        // incorrectly consume the next line as arguments to a no-arg method call!
        RE2 withArgsRegex("(?im)(^[ \\t]*|:[ \\t]*|\\bThen[ \\t]+|\\bElse[ \\t]+)" + escapedName + "[ \\t]+([^=:\\r\\n][^:\\r\\n]*)");

        std::string temp;
        auto matches = RE2FindAll(result, withArgsRegex);
        size_t lastPos = 0;

        for (const auto& match : matches) {
            size_t matchPos = match.position;

            // Check if already transformed (preceded by class name)
            bool alreadyTransformed = false;
            if (matchPos > classDef.name.length() + 1) {
                std::string before = result.substr(matchPos, classDef.name.length() + 1);
                if (before.find(classDef.name + "_") != std::string::npos) {
                    alreadyTransformed = true;
                }
            }

            // Check if inside a string literal
            int quoteCount = 0;
            for (size_t i = 0; i < matchPos; ++i) {
                if (result[i] == '"') quoteCount++;
            }
            if (quoteCount % 2 == 1) {
                alreadyTransformed = true;
            }

            // Check if inside a VBScript comment
            if (!alreadyTransformed && isInsideComment(result, matchPos)) {
                alreadyTransformed = true;
            }

            // Check if "args" actually start with a VBS keyword - means this is a no-args
            // call followed by a new statement (e.g., "MethodName\nIf x Then" matched wrongly)
            std::string args = (match.groups.size() > 1) ? match.groups[1] : "";
            static const RE2 vbsKeywordStart(R"((?i)^(If|For|Do|While|Select|With|End|Exit|Sub|Function|Dim|Const|Set|Call|On|Rem|Loop|Next|Wend|ElseIf|Case|Class|Private|Public|Property|ReDim|Erase|Execute|ExecuteGlobal|Randomize|Option)\b)");
            if (RE2::PartialMatch(args, vbsKeywordStart)) {
                alreadyTransformed = true;  // Skip - this isn't really args
            }

            temp += result.substr(lastPos, matchPos - lastPos);
            if (alreadyTransformed) {
                temp += match.full_match;
            } else {
                temp += match[1] + classDef.name + "_" + method.name + " this_, " + args;
            }
            lastPos = matchPos + match.length;
        }
        temp += result.substr(lastPos);
        if (!temp.empty()) result = temp;

        // Pattern for method call without arguments: methodName (standalone, followed by newline or :)
        // RE2 doesn't support lookahead, so capture trailing boundary and restore it
        RE2 noArgsRegex("(?im)(^[ \\t]*|:[ \\t]*|\\bThen[ \\t]+|\\bElse[ \\t]+)" + escapedName + "([ \\t]*(?::|\\r|\\n|$))");

        temp.clear();
        matches = RE2FindAll(result, noArgsRegex);
        lastPos = 0;

        for (const auto& match : matches) {
            size_t matchPos = match.position;

            // Check if already transformed or inside string
            bool skip = false;
            int quoteCount = 0;
            for (size_t i = 0; i < matchPos; ++i) {
                if (result[i] == '"') quoteCount++;
            }
            if (quoteCount % 2 == 1) {
                skip = true;
            }

            // Check if inside a VBScript comment
            if (!skip && isInsideComment(result, matchPos)) {
                skip = true;
            }

            temp += result.substr(lastPos, matchPos - lastPos);
            if (skip) {
                temp += match.full_match;
            } else {
                // Group 1 is prefix, group 2 is trailing boundary - restore it
                std::string trailing = (match.groups.size() > 1) ? match.groups[1] : "";
                temp += match[1] + classDef.name + "_" + method.name + " this_" + trailing;
            }
            lastPos = matchPos + match.length;
        }
        temp += result.substr(lastPos);
        if (!temp.empty()) result = temp;

        // Pattern for function-style call with no arguments: methodName() in expressions
        // E.g., If FlipperOn() Then -> If FlipperPolarity_FlipperOn(this_) Then
        RE2 funcCallRegex("(?i)\\b" + escapedName + "\\s*\\(\\s*\\)");

        temp.clear();
        matches = RE2FindAll(result, funcCallRegex);
        lastPos = 0;

        for (const auto& match : matches) {
            size_t matchPos = match.position;

            // Check if already transformed (preceded by class name_)
            bool skip = false;
            if (matchPos >= classDef.name.length() + 1) {
                std::string before = result.substr(matchPos - classDef.name.length() - 1, classDef.name.length() + 1);
                if (before == classDef.name + "_") {
                    skip = true;
                }
            }

            // Check if inside a string literal
            int quoteCount = 0;
            for (size_t i = 0; i < matchPos; ++i) {
                if (result[i] == '"') quoteCount++;
            }
            if (quoteCount % 2 == 1) {
                skip = true;
            }

            // Check if inside a VBScript comment
            if (!skip && isInsideComment(result, matchPos)) {
                skip = true;
            }

            temp += result.substr(lastPos, matchPos - lastPos);
            if (skip) {
                temp += match.full_match;
            } else {
                temp += classDef.name + "_" + method.name + "(this_)";
            }
            lastPos = matchPos + match.length;
        }
        temp += result.substr(lastPos);
        if (!temp.empty()) result = temp;
    }

    // Transform accessor calls within method bodies
    // Property Let: accessor(params) = value -> ClassName_Let_accessor this_, params, value
    // Property Get: accessor(params) -> ClassName_Get_accessor(this_, params)
    // IMPORTANT: Skip if preceded by a dot - that means it's being called on a different object!
    for (const auto& accessor : classDef.accessors) {
        std::string escapedName = EscapeRegex(accessor.name);

        if (EqualsIgnoreCase(accessor.type, "Let") || EqualsIgnoreCase(accessor.type, "Set")) {
            // Transform: accessor(params) = value -> ClassName_Let_accessor this_, params, value
            // Skip if preceded by dot (accessing accessor on another object)
            RE2 letRegex("(?i)\\b" + escapedName + "\\s*\\(([^)]+)\\)\\s*=\\s*(.+)");

            std::string letTemp;
            auto letMatches = RE2FindAll(result, letRegex);
            size_t letLastPos = 0;

            for (const auto& match : letMatches) {
                size_t matchPos = match.position;
                bool skip = false;

                // Skip if preceded by a dot (means obj.accessor, not standalone accessor)
                if (matchPos > 0 && result[matchPos - 1] == '.') {
                    skip = true;
                }

                letTemp += result.substr(letLastPos, matchPos - letLastPos);
                if (skip) {
                    letTemp += match.full_match;
                } else {
                    std::string params = match.groups.size() > 0 ? match.groups[0] : "";
                    std::string value = match.groups.size() > 1 ? match.groups[1] : "";
                    letTemp += classDef.name + "_" + accessor.type + "_" + accessor.name + " this_, " + params + ", " + value;
                }
                letLastPos = matchPos + match.length;
            }
            letTemp += result.substr(letLastPos);
            if (!letTemp.empty()) result = letTemp;

            // Transform: accessor = value -> ClassName_Let_accessor this_, value
            // For accessors without index parameters (just the value param)
            // Must be at start of statement (after newline+optional indent, colon+space, or Then+space)
            //// Pattern: (^|\n[ \t]*|:[ \t]*|\bThen[ \t]+)accessor = value
            RE2 letNoParamRegex("(?i)(^|\\n[ \\t]*|:[ \\t]*|\\bThen[ \\t]+)" + escapedName + "\\s*=\\s*([^:\\r\\n]+)");
            result = RE2Replace(result, letNoParamRegex,
                "\\1" + classDef.name + "_" + accessor.type + "_" + accessor.name + " this_, \\2");
        }

        if (EqualsIgnoreCase(accessor.type, "Get")) {
            // Transform: accessor(params) -> ClassName_Get_accessor(this_, params)
            // Be careful not to match our own transformed Let calls
            // Skip if preceded by dot (accessing accessor on another object)
            // Skip if followed by = (that's an assignment, handled by Let transformation)
            RE2 getRegex("(?i)\\b" + escapedName + "\\s*\\(([^)]+)\\)");

            // Use a callback-style replacement to avoid matching already-transformed calls
            std::string temp;
            auto matches = RE2FindAll(result, getRegex);
            size_t lastPos = 0;

            for (const auto& match : matches) {
                size_t matchPos = match.position;
                bool skip = false;

                // Skip if preceded by a dot (means obj.accessor, not standalone accessor)
                if (matchPos > 0 && result[matchPos - 1] == '.') {
                    skip = true;
                }

                // Check if this is preceded by our class name (already transformed)
                if (!skip && matchPos > classDef.name.length() + 1) {
                    std::string before = result.substr(matchPos - classDef.name.length() - 1, classDef.name.length() + 1);
                    if (before.find(classDef.name + "_") != std::string::npos) {
                        skip = true;
                    }
                }

                // Skip if followed by = (this is an assignment, Let transformation handles it)
                // e.g., state(idx) = value should NOT become Get_state, Let handles it
                if (!skip) {
                    size_t afterPos = matchPos + match.length;
                    while (afterPos < result.length() && (result[afterPos] == ' ' || result[afterPos] == '\t')) {
                        afterPos++;
                    }
                    if (afterPos < result.length() && result[afterPos] == '=') {
                        skip = true;
                    }
                }

                temp += result.substr(lastPos, matchPos - lastPos);
                if (skip) {
                    temp += match.full_match;
                } else {
                    temp += classDef.name + "_Get_" + accessor.name + "(this_, " + match[1] + ")";
                }
                lastPos = matchPos + match.length;
            }
            temp += result.substr(lastPos);
            if (!temp.empty()) result = temp;

            // Transform: accessor -> ClassName_Get_accessor(this_)
            // For parameterless Property Get - must not be followed by ( or =
            // Must not be part of an already-transformed call (preceded by ClassName_)
            // IMPORTANT: Skip if this matches a method parameter name!
            // Note: RE2 doesn't support (?!) lookahead, so we check manually
            RE2 getNoParamRegex("(?i)\\b" + escapedName + "\\b");

            std::string temp2;
            matches = RE2FindAll(result, getNoParamRegex);
            size_t lastPos2 = 0;

            for (const auto& match : matches) {
                size_t matchPos = match.position;
                bool skip = false;

                // Skip if this matches a method parameter name
                if (isMethodParam(accessor.name)) {
                    skip = true;
                }

                // Check if followed by ( or =
                if (!skip && matchPos + match.length < result.length()) {
                    size_t afterPos = matchPos + match.length;
                    while (afterPos < result.length() && (result[afterPos] == ' ' || result[afterPos] == '\t')) {
                        afterPos++;
                    }
                    if (afterPos < result.length() && (result[afterPos] == '(' || result[afterPos] == '=')) {
                        skip = true;
                    }
                }

                // Check if preceded by class name_ (already transformed)
                if (!skip && matchPos > classDef.name.length() + 1) {
                    std::string before = result.substr(matchPos - classDef.name.length() - 1, classDef.name.length() + 1);
                    if (before.find(classDef.name + "_") != std::string::npos) {
                        skip = true;
                    }
                }

                temp2 += result.substr(lastPos2, matchPos - lastPos2);
                if (skip) {
                    temp2 += match.full_match;
                } else {
                    temp2 += classDef.name + "_Get_" + accessor.name + "(this_)";
                }
                lastPos2 = matchPos + match.length;
            }
            temp2 += result.substr(lastPos2);
            if (!temp2.empty()) result = temp2;
        }
    }

    // Transform callback string patterns for event handlers
    // Pattern: varname & ".MethodName args" -> "ClassName_MethodName " & varname & ", args"
    // This handles vpmBuildEvent calls like: aName & ".AddBall ActiveBall"
    for (const auto& method : classDef.methods) {
        std::string escapedMethodName = EscapeRegex(method.name);
        // Pattern: (varname) & ".(MethodName)( args)?"
        // Match: someVar & ".MethodName" or someVar & ".MethodName arg1 arg2"
        RE2 callbackRegex("(?i)(\\w+)\\s*&\\s*\"\\." + escapedMethodName + "(?:\\s+([^\"]+))?\"");

        std::string temp;
        auto matches = RE2FindAll(result, callbackRegex);
        size_t lastPos = 0;

        for (const auto& match : matches) {
            size_t matchPos = match.position;
            temp += result.substr(lastPos, matchPos - lastPos);

            std::string varName = match[1];
            std::string args = match.groups.size() > 1 ? match.groups[1] : "";

            // Build replacement: "ClassName_MethodName " & varname & ", args"
            std::string replacement = "\"" + classDef.name + "_" + method.name + " \" & " + varName;
            if (!args.empty()) {
                replacement += " & \", " + args + "\"";
            }
            temp += replacement;
            lastPos = matchPos + match.length;
        }
        temp += result.substr(lastPos);
        if (!temp.empty()) result = temp;
    }

    return result;
}


std::string ScriptPatcher::EmitClassEmulation(const VBClassDefinition& classDef) {
    std::ostringstream out;
    out << "' === " << classDef.name << " Class Emulation ===\n";
    // Note: Array declarations are injected at the TOP of the script by EmulateClasses()
    out << "\n";

    // Factory function
    out << "Function " << classDef.name << "_Create()\n";
    out << "    Dim this_\n";
    out << "    Set this_ = CreateObject(\"Scripting.Dictionary\")\n";
    out << "    this_(\"__class__\") = \"" << classDef.name << "\"\n";
    // Only non-array properties go in Dictionary (arrays are global)
    for (const auto& prop : classDef.properties) {
        if (!prop.isArray) {
            out << "    this_(\"" << prop.name << "\") = Empty\n";
        }
    }

    if (!classDef.initializeBody.empty()) {
        out << "    ' Class_Initialize\n";
        std::string initBody = TransformMethodBody(classDef.initializeBody, classDef);
        std::istringstream initStream(initBody);
        std::string initLine;
        while (std::getline(initStream, initLine))
            if (!Trim(initLine).empty()) out << "    " << initLine << "\n";
    }
    out << "    Set " << classDef.name << "_Create = this_\n";
    out << "End Function\n\n";

    // Methods
    for (const auto& method : classDef.methods) {
        std::string paramList = "this_";
        for (const auto& p : method.params) paramList += ", " + p;

        std::string funcName = classDef.name + "_" + method.name;
        out << (method.isFunction ? "Function " : "Sub ") << funcName
            << "(" << paramList << ")\n";
        std::string transformedBody = TransformMethodBody(method.body, classDef, method.params);

        // For Functions, transform return value assignment: methodName = value -> FuncName = value
        if (method.isFunction) {
            std::string escapedMethodName = EscapeRegex(method.name);
            RE2 returnPattern("(?i)(^|:|\\s)" + escapedMethodName + "\\s*=");
            RE2 setReturnPattern("(?i)(^|:|\\s)(Set\\s+)" + escapedMethodName + "\\s*=");
            transformedBody = RE2Replace(transformedBody, setReturnPattern, "\\1\\2" + funcName + " =");
            transformedBody = RE2Replace(transformedBody, returnPattern, "\\1" + funcName + " =");
        }

        std::istringstream bodyStream(transformedBody);
        std::string bodyLine;
        while (std::getline(bodyStream, bodyLine)) out << "    " << bodyLine << "\n";
        out << (method.isFunction ? "End Function\n\n" : "End Sub\n\n");
    }

    // Accessors (skip empty ones only)
    for (const auto& accessor : classDef.accessors) {
        if (Trim(accessor.body).empty()) continue;
        std::string paramList = "this_";
        for (const auto& p : accessor.params) paramList += ", " + p;

        if (EqualsIgnoreCase(accessor.type, "Get")) {
            std::string funcName = classDef.name + "_Get_" + accessor.name;
            out << "Function " << funcName << "(" << paramList << ")\n";
            // IMPORTANT: Transform return value BEFORE TransformMethodBody
            // Otherwise, the accessor-without-params transform will convert
            // "state = value" to "ClassName_Let_state this_, value" instead of return
            // In VBScript Property Get, you assign to the property name to return
            // Handle both "accessorName =" and "Set accessorName ="
            std::string escapedAccessorName = EscapeRegex(accessor.name);
            RE2 returnPattern("(?i)(^|:|\\s)" + escapedAccessorName + "\\s*=");
            RE2 setReturnPattern("(?i)(^|:|\\s)(Set\\s+)" + escapedAccessorName + "\\s*=");
            std::string bodyWithReturn = accessor.body;
            bodyWithReturn = RE2Replace(bodyWithReturn, setReturnPattern, "\\1\\2" + funcName + " =");
            bodyWithReturn = RE2Replace(bodyWithReturn, returnPattern, "\\1" + funcName + " =");
            // Now transform the rest of the body
            std::string tb = TransformMethodBody(bodyWithReturn, classDef, accessor.params);
            std::istringstream s(tb); std::string l;
            while (std::getline(s, l)) out << "    " << l << "\n";
            out << "End Function\n\n";
        } else {
            out << "Sub " << classDef.name << "_" << accessor.type << "_" << accessor.name << "(" << paramList << ")\n";
            std::string tb = TransformMethodBody(accessor.body, classDef, accessor.params);
            std::istringstream s(tb); std::string l;
            while (std::getline(s, l)) out << "    " << l << "\n";
            out << "End Sub\n\n";
        }
    }
    out << "' === End " << classDef.name << " ===\n\n";
    return out.str();
}

// ============================================================================
// CLASS EMULATION - Phase 3: Usage Transformation
// ============================================================================

#endif // __STANDALONE__
