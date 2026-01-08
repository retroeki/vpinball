/**
 * @file scriptpatcher_classes_emit.cpp
 * @brief VBScript Class Emulation Code Generation
 *
 * Generates Wine-compatible VBScript code from parsed class definitions:
 * - ClassName_Create() factory functions
 * - ClassName_MethodName() transformed methods
 * - ClassName_Get/Let_AccessorName() property accessors
 * - TransformMethodBody: converts Me.X to this_("X"), etc.
 */

#include "stdafx.h"

#ifdef __STANDALONE__

#include "scriptpatcher.h"
#include "scriptpatcher_internal.h"
#include <regex>
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

    // Me.Property -> this_("Property")
    static std::regex meDotPattern(R"(\bMe\.(\w+))", std::regex::icase);
    result = std::regex_replace(result, meDotPattern, "this_(\"$1\")");
    // Standalone Me -> this_
    static std::regex mePattern(R"(\bMe\b(?!\.))", std::regex::icase);
    result = std::regex_replace(result, mePattern, "this_");

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
                std::string mePropPattern = "\\b(Me|this_)\\." + escapedName + "\\b";
                std::regex mePropRegex(mePropPattern, std::regex::icase);
                result = std::regex_replace(result, mePropRegex, globalName);
            } else {
                // Transform any object.PropName (class-specific array, aObj must be same class)
                std::string objPropPattern = "\\b\\w+\\." + escapedName + "\\b";
                std::regex objPropRegex(objPropPattern, std::regex::icase);
                result = std::regex_replace(result, objPropRegex, globalName);
            }

            // Then replace standalone PropName -> ClassName_PropName
            std::string propPattern = "\\b" + escapedName + "\\b";
            std::regex allPattern(propPattern, std::regex::icase);
            {
                std::string temp;
                std::sregex_iterator it(result.begin(), result.end(), allPattern);
                std::sregex_iterator end;
                size_t lastPos = 0;
                for (; it != end; ++it) {
                    std::smatch match = *it;
                    size_t matchPos = match.position();
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
                        temp += match[0].str();
                    } else {
                        temp += globalName;
                    }
                    lastPos = matchPos + match[0].length();
                }
                temp += result.substr(lastPos);
                if (!temp.empty()) result = temp;
            }
        } else {
            // Non-array properties: use Dictionary-based approach
            // First transform assignments: prop = value -> this_("prop") = value
            // Use iterative approach to skip matches preceded by a dot (e.g., aTrigger.Name = x)
            std::string assignPattern = "\\b" + escapedName + "(\\s*=\\s*)";
            std::regex assignRegex(assignPattern, std::regex::icase);
            {
                std::string assignTemp;
                std::sregex_iterator ait(result.begin(), result.end(), assignRegex);
                std::sregex_iterator aend;
                size_t aLastPos = 0;
                for (; ait != aend; ++ait) {
                    std::smatch amatch = *ait;
                    size_t aMatchPos = amatch.position();
                    // Check if preceded by dot
                    bool aSkip = (aMatchPos > 0 && result[aMatchPos - 1] == '.');
                    assignTemp += result.substr(aLastPos, aMatchPos - aLastPos);
                    if (aSkip) {
                        assignTemp += amatch[0].str();  // Keep original
                    } else {
                        assignTemp += "this_(\"" + prop.name + "\")" + amatch[1].str();
                    }
                    aLastPos = aMatchPos + amatch[0].length();
                }
                assignTemp += result.substr(aLastPos);
                if (!assignTemp.empty()) result = assignTemp;
            }

            // Then transform reads: prop -> this_("prop")
            // Skip matches already inside this_("...") or inside string literals
            std::string readPattern = "\\b" + escapedName + "\\b";
            std::regex readRegex(readPattern, std::regex::icase);

            std::string temp;
            std::sregex_iterator it(result.begin(), result.end(), readRegex);
            std::sregex_iterator end;
            size_t lastPos = 0;

            for (; it != end; ++it) {
                std::smatch match = *it;
                size_t matchPos = match.position();

                // Check if this is inside this_("...") - already transformed
                bool skip = false;
                if (matchPos >= 7) {
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

                temp += result.substr(lastPos, matchPos - lastPos);
                if (skip) {
                    temp += match[0].str();  // Keep original
                } else {
                    temp += "this_(\"" + prop.name + "\")";
                }
                lastPos = matchPos + match[0].length();
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
        // Exclude function return assignments (methodName = value) using negative lookahead
        std::string withArgsPattern = "(^[ \\t]*|:[ \\t]*|\\bThen[ \\t]+|\\bElse[ \\t]+)" + escapedName + "\\s+(?!=)([^:\\r\\n]+)";
        std::regex withArgsRegex(withArgsPattern, std::regex::icase | std::regex::multiline);

        std::string temp;
        std::sregex_iterator it(result.begin(), result.end(), withArgsRegex);
        std::sregex_iterator end;
        size_t lastPos = 0;

        while (it != end) {
            std::smatch match = *it;
            size_t matchPos = match.position();

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

            temp += result.substr(lastPos, matchPos - lastPos);
            if (alreadyTransformed) {
                temp += match[0].str();
            } else {
                temp += match[1].str() + classDef.name + "_" + method.name + " this_, " + match[2].str();
            }
            lastPos = matchPos + match[0].length();
            ++it;
        }
        temp += result.substr(lastPos);
        if (!temp.empty()) result = temp;

        // Pattern for method call without arguments: methodName (standalone, followed by newline or :)
        std::string noArgsPattern = "(^[ \\t]*|:[ \\t]*|\\bThen[ \\t]+|\\bElse[ \\t]+)" + escapedName + "(?=[ \\t]*(?::|\\r|\\n|$))";
        std::regex noArgsRegex(noArgsPattern, std::regex::icase | std::regex::multiline);

        temp.clear();
        std::sregex_iterator it2(result.begin(), result.end(), noArgsRegex);
        lastPos = 0;

        while (it2 != end) {
            std::smatch match = *it2;
            size_t matchPos = match.position();

            // Check if already transformed or inside string
            bool skip = false;
            int quoteCount = 0;
            for (size_t i = 0; i < matchPos; ++i) {
                if (result[i] == '"') quoteCount++;
            }
            if (quoteCount % 2 == 1) {
                skip = true;
            }

            temp += result.substr(lastPos, matchPos - lastPos);
            if (skip) {
                temp += match[0].str();
            } else {
                temp += match[1].str() + classDef.name + "_" + method.name + " this_";
            }
            lastPos = matchPos + match[0].length();
            ++it2;
        }
        temp += result.substr(lastPos);
        if (!temp.empty()) result = temp;

        // Pattern for function-style call with no arguments: methodName() in expressions
        // E.g., If FlipperOn() Then -> If FlipperPolarity_FlipperOn(this_) Then
        std::string funcCallPattern = "\\b" + escapedName + "\\s*\\(\\s*\\)";
        std::regex funcCallRegex(funcCallPattern, std::regex::icase);

        temp.clear();
        std::sregex_iterator it3(result.begin(), result.end(), funcCallRegex);
        lastPos = 0;

        while (it3 != end) {
            std::smatch match = *it3;
            size_t matchPos = match.position();

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

            temp += result.substr(lastPos, matchPos - lastPos);
            if (skip) {
                temp += match[0].str();
            } else {
                temp += classDef.name + "_" + method.name + "(this_)";
            }
            lastPos = matchPos + match[0].length();
            ++it3;
        }
        temp += result.substr(lastPos);
        if (!temp.empty()) result = temp;
    }

    // Transform accessor calls within method bodies
    // Property Let: accessor(params) = value -> ClassName_Let_accessor this_, params, value
    // Property Get: accessor(params) -> ClassName_Get_accessor(this_, params)
    for (const auto& accessor : classDef.accessors) {
        std::string escapedName = EscapeRegex(accessor.name);

        if (EqualsIgnoreCase(accessor.type, "Let") || EqualsIgnoreCase(accessor.type, "Set")) {
            // Transform: accessor(params) = value -> ClassName_Let_accessor this_, params, value
            //// Pattern: accessor(params) = value
            std::string letPattern = "\\b" + escapedName + "\\s*\\(([^)]+)\\)\\s*=\\s*(.+)";
            std::regex letRegex(letPattern, std::regex::icase);
            result = std::regex_replace(result, letRegex,
                classDef.name + "_" + accessor.type + "_" + accessor.name + " this_, $1, $2");

            // Transform: accessor = value -> ClassName_Let_accessor this_, value
            // For accessors without index parameters (just the value param)
            // Must be at start of statement (after newline, colon+space, or Then+space)
            //// Pattern: (^|\n|:[ \t]*|\bThen[ \t]+)accessor = value
            std::string letNoParamPattern = "(^|\\n|:[ \\t]*|\\bThen[ \\t]+)" + escapedName + "\\s*=\\s*([^:\\r\\n]+)";
            std::regex letNoParamRegex(letNoParamPattern, std::regex::icase);
            result = std::regex_replace(result, letNoParamRegex,
                "$1" + classDef.name + "_" + accessor.type + "_" + accessor.name + " this_, $2");
        }

        if (EqualsIgnoreCase(accessor.type, "Get")) {
            // Transform: accessor(params) -> ClassName_Get_accessor(this_, params)
            // Be careful not to match our own transformed Let calls
            std::string getPattern = "\\b" + escapedName + "\\s*\\(([^)]+)\\)";
            std::regex getRegex(getPattern, std::regex::icase);
            // Only replace if not already transformed (not preceded by class name)
            std::string replacement = classDef.name + "_Get_" + accessor.name + "(this_, $1)";

            // Use a callback-style replacement to avoid matching already-transformed calls
            std::string temp;
            std::sregex_iterator it(result.begin(), result.end(), getRegex);
            std::sregex_iterator end;
            size_t lastPos = 0;

            for (; it != end; ++it) {
                std::smatch match = *it;
                // Check if this is preceded by our class name (already transformed)
                size_t matchPos = match.position();
                bool alreadyTransformed = false;
                if (matchPos > classDef.name.length() + 1) {
                    std::string before = result.substr(matchPos - classDef.name.length() - 1, classDef.name.length() + 1);
                    if (before.find(classDef.name + "_") != std::string::npos) {
                        alreadyTransformed = true;
                    }
                }

                temp += result.substr(lastPos, matchPos - lastPos);
                if (alreadyTransformed) {
                    temp += match[0].str();
                } else {
                    temp += classDef.name + "_Get_" + accessor.name + "(this_, " + match[1].str() + ")";
                }
                lastPos = matchPos + match[0].length();
            }
            temp += result.substr(lastPos);
            if (!temp.empty()) result = temp;

            // Transform: accessor -> ClassName_Get_accessor(this_)
            // For parameterless Property Get - must not be followed by ( or =
            // Must not be part of an already-transformed call (preceded by ClassName_)
            // IMPORTANT: Skip if this matches a method parameter name!
            std::string getNoParamPattern = "\\b" + escapedName + "\\b(?!\\s*[=(])";
            std::regex getNoParamRegex(getNoParamPattern, std::regex::icase);

            std::string temp2;
            std::sregex_iterator it2(result.begin(), result.end(), getNoParamRegex);
            std::sregex_iterator end2;
            size_t lastPos2 = 0;

            for (; it2 != end2; ++it2) {
                std::smatch match = *it2;
                size_t matchPos = match.position();
                bool skip = false;

                // Skip if this matches a method parameter name
                if (isMethodParam(accessor.name)) {
                    skip = true;
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
                    temp2 += match[0].str();
                } else {
                    temp2 += classDef.name + "_Get_" + accessor.name + "(this_)";
                }
                lastPos2 = matchPos + match[0].length();
            }
            temp2 += result.substr(lastPos2);
            if (!temp2.empty()) result = temp2;
        }
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
            std::regex returnPattern("(^|:|\\s)" + escapedMethodName + "\\s*=", std::regex::icase);
            std::regex setReturnPattern("(^|:|\\s)(Set\\s+)" + escapedMethodName + "\\s*=", std::regex::icase);
            transformedBody = std::regex_replace(transformedBody, setReturnPattern, "$1$2" + funcName + " =");
            transformedBody = std::regex_replace(transformedBody, returnPattern, "$1" + funcName + " =");
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
            std::regex returnPattern("(^|:|\\s)" + escapedAccessorName + "\\s*=", std::regex::icase);
            std::regex setReturnPattern("(^|:|\\s)(Set\\s+)" + escapedAccessorName + "\\s*=", std::regex::icase);
            std::string bodyWithReturn = accessor.body;
            bodyWithReturn = std::regex_replace(bodyWithReturn, setReturnPattern, "$1$2" + funcName + " =");
            bodyWithReturn = std::regex_replace(bodyWithReturn, returnPattern, "$1" + funcName + " =");
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
