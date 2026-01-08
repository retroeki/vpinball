/**
 * @file scriptpatcher_classes.cpp
 * @brief VBScript Class Emulation for Wine/Android compatibility
 *
 * Wine's VBScript engine doesn't support the "Class" keyword. This module
 * transforms VBScript classes into Dictionary-based objects that Wine can handle.
 *
 * ORIGINAL:                          TRANSFORMED:
 * ---------                          ------------
 * Class Foo                          Function Foo_Create()
 *     Public Value                       Dim this_
 *     Public Sub Init(x)                 Set this_ = CreateObject("Scripting.Dictionary")
 *         Value = x                      this_("Value") = Empty
 *     End Sub                            Set Foo_Create = this_
 * End Class                          End Function
 *
 * Set obj = New Foo                  Sub Foo_Init(this_, x)
 * obj.Init 5                             this_("Value") = x
 * x = obj.Value                      End Sub
 *
 *                                    Set obj = Foo_Create()
 *                                    Foo_Init obj, 5
 *                                    x = obj("Value")
 */

#include "stdafx.h"

#ifdef __STANDALONE__

#include "scriptpatcher.h"
#include "scriptpatcher_internal.h"
#include <regex>
#include <sstream>
#include <algorithm>

// ============================================================================
// CLASS EMULATION - Phase 1: Parsing
// ============================================================================

bool ScriptPatcher::HasClassDefinitions(const std::string& script) {
    static std::regex classPattern(R"(\bClass\s+\w+)", std::regex::icase);
    return std::regex_search(script, classPattern);
}


std::vector<std::string> ScriptPatcher::ParseParameters(const std::string& paramStr) {
    std::vector<std::string> params;
    std::string trimmed = Trim(paramStr);
    if (trimmed.empty()) return params;
    
    std::istringstream stream(trimmed);
    std::string token;
    while (std::getline(stream, token, ',')) {
        token = Trim(token);
        static std::regex byvalRef(R"(^(ByVal|ByRef)\s+)", std::regex::icase);
        token = std::regex_replace(token, byvalRef, "");
        token = Trim(token);
        if (!token.empty()) params.push_back(token);
    }
    return params;
}


std::vector<VBClassDefinition> ScriptPatcher::ParseClassDefinitions(const std::string& script) {
    std::vector<VBClassDefinition> classes;
    std::istringstream stream(script);
    std::string line;
    size_t pos = 0;
    
    bool inClass = false;
    VBClassDefinition currentClass;
    bool inMethod = false;
    VBClassMethod currentMethod;
    int methodNestLevel = 0;
    bool inAccessor = false;
    VBClassAccessor currentAccessor;
    int accessorNestLevel = 0;
    
    static std::regex classStartPattern(R"(^\s*Class\s+(\w+))", std::regex::icase);
    static std::regex classEndPattern(R"(^\s*End\s+Class\s*$)", std::regex::icase);
    static std::regex propertyDeclPattern(R"(^\s*(Public|Private)\s+(?!Sub|Function|Property|Default)(.+)$)", std::regex::icase);
    static std::regex methodStartPattern(R"(^\s*(Public\s+|Private\s+)?(Default\s+)?(Sub|Function)\s+(\w+)(?:\s*\(([^)]*)\))?)", std::regex::icase);
    static std::regex methodEndSubPattern(R"(^\s*End\s+Sub\s*$)", std::regex::icase);
    static std::regex methodEndFuncPattern(R"(^\s*End\s+Function\s*$)", std::regex::icase);
    static std::regex accessorStartPattern(R"(^\s*(Public\s+|Private\s+)?Property\s+(Get|Let|Set)\s+(\w+)(?:\s*\(([^)]*)\))?)", std::regex::icase);
    static std::regex accessorEndPattern(R"(^\s*End\s+Property\s*$)", std::regex::icase);
    // Nest patterns - match at line start (with whitespace) OR after statement separator (:)
    // For inline blocks like "Dim x : For x = 0 to N"
    // Use ^\s* to allow leading whitespace at line start
    static std::regex nestStartPattern(R"((^\s*|:\s*)(If\s+.*\s+Then\s*$|For\s+|Do\s+|While\s+|Select\s+Case))", std::regex::icase);
    static std::regex nestEndPattern(R"((^\s*|:\s*)(End\s+If|Next|Loop|Wend|End\s+Select))", std::regex::icase);

    while (std::getline(stream, line)) {
        std::smatch match;
        size_t lineStart = pos;
        size_t originalLen = line.length();  // Before stripping \r

        // Remove carriage return if present (Windows CRLF line endings)
        // Must capture length BEFORE stripping for correct position tracking
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        pos += originalLen + 1;  // Use original length + 1 for the \n
        std::string trimmedLine = Trim(line);
        
        if (trimmedLine.empty() || trimmedLine[0] == '\'') {
            if (inMethod) currentMethod.body += line + "\n";
            if (inAccessor) currentAccessor.body += line + "\n";
            continue;
        }
        
        if (!inClass && std::regex_search(line, match, classStartPattern)) {
            // Check for single-line class definition (Class X : ... : End Class on same line)
            static std::regex singleLineClassPattern(R"(\bEnd\s+Class\b)", std::regex::icase);
            if (std::regex_search(line, singleLineClassPattern)) {
                // Single-line stub class - skip it entirely (don't emulate)
                PLOGI.printf("ScriptPatcher: Skipping single-line stub class '%s'", match[1].str().c_str());
                continue;
            }
            inClass = true;
            inMethod = false;  // Reset method state when entering new class
            inAccessor = false;  // Reset accessor state when entering new class
            methodNestLevel = 0;
            accessorNestLevel = 0;
            currentClass = VBClassDefinition();
            currentClass.name = match[1].str();
            currentClass.startPos = lineStart;
            PLOGI.printf("ScriptPatcher: Parsing class '%s'", currentClass.name.c_str());
            continue;
        }

        if (inClass && std::regex_search(line, match, classEndPattern)) {
            currentClass.endPos = pos;
            // Count array properties for logging
            size_t arrayPropCount = 0;
            for (const auto& p : currentClass.properties) {
                if (p.isArray) arrayPropCount++;
            }
            PLOGI.printf("ScriptPatcher: Class '%s' has %zu methods, %zu accessors, %zu properties (%zu arrays), initBody=%zu chars",
                currentClass.name.c_str(), currentClass.methods.size(), currentClass.accessors.size(),
                currentClass.properties.size(), arrayPropCount, currentClass.initializeBody.length());
            classes.push_back(currentClass);
            inClass = false;
            inMethod = false;  // Reset state on class end
            inAccessor = false;
            methodNestLevel = 0;
            accessorNestLevel = 0;
            continue;
        }
        
        if (!inClass) continue;
        
        if (inMethod) {
            bool isEndSub = std::regex_search(line, methodEndSubPattern);
            bool isEndFunc = std::regex_search(line, methodEndFuncPattern);
            if ((isEndSub && !currentMethod.isFunction) || (isEndFunc && currentMethod.isFunction)) {
                if (methodNestLevel == 0) {
                    if (EqualsIgnoreCase(currentMethod.name, "Class_Initialize"))
                        currentClass.initializeBody = currentMethod.body;
                    else if (EqualsIgnoreCase(currentMethod.name, "Class_Terminate"))
                        currentClass.terminateBody = currentMethod.body;
                    else
                        currentClass.methods.push_back(currentMethod);
                    inMethod = false;
                    continue;
                }
            }
            if (std::regex_search(line, nestStartPattern)) methodNestLevel++;
            if (std::regex_search(line, nestEndPattern) && methodNestLevel > 0) methodNestLevel--;
            currentMethod.body += line + "\n";
            continue;
        }

        if (inAccessor) {
            if (std::regex_search(line, accessorEndPattern)) {
                if (accessorNestLevel == 0) {
                    currentClass.accessors.push_back(currentAccessor);
                    inAccessor = false;
                    continue;
                }
            }
            if (std::regex_search(line, nestStartPattern)) accessorNestLevel++;
            if (std::regex_search(line, nestEndPattern) && accessorNestLevel > 0) accessorNestLevel--;
            currentAccessor.body += line + "\n";
            continue;
        }
        
        if (std::regex_search(line, match, methodStartPattern)) {
            currentMethod = VBClassMethod();
            std::string visibility = match[1].str();
            currentMethod.isPublic = visibility.empty() || visibility.find("Public") != std::string::npos;
            currentMethod.isDefault = !match[2].str().empty();
            currentMethod.isFunction = EqualsIgnoreCase(Trim(match[3].str()), "Function");
            currentMethod.name = match[4].str();
            currentMethod.params = ParseParameters(match[5].str());
            currentMethod.body = "";

            // Check for single-line method: Sub Foo() : body : End Sub
            // Pattern to find : ... : End Sub or : ... : End Function on same line
            // Allow trailing comments ('...) after End Sub/Function
            static std::regex singleLineEndSub(R"(:\s*(.*):\s*End\s+Sub\s*('.*)?$)", std::regex::icase);
            static std::regex singleLineEndFunc(R"(:\s*(.*):\s*End\s+Function\s*('.*)?$)", std::regex::icase);
            std::smatch singleLineMatch;
            bool isSingleLine = false;

            if (!currentMethod.isFunction && std::regex_search(line, singleLineMatch, singleLineEndSub)) {
                currentMethod.body = Trim(singleLineMatch[1].str());
                isSingleLine = true;
            } else if (currentMethod.isFunction && std::regex_search(line, singleLineMatch, singleLineEndFunc)) {
                currentMethod.body = Trim(singleLineMatch[1].str());
                isSingleLine = true;
            }

            if (currentClass.name == "VPMLampUpdater") {
                PLOGI.printf("ScriptPatcher: VPMLampUpdater found method '%s' (singleLine=%d)", currentMethod.name.c_str(), isSingleLine);
            }

            if (isSingleLine) {
                // Single-line method - add immediately, don't enter multi-line mode
                if (EqualsIgnoreCase(currentMethod.name, "Class_Initialize"))
                    currentClass.initializeBody = currentMethod.body;
                else if (EqualsIgnoreCase(currentMethod.name, "Class_Terminate"))
                    currentClass.terminateBody = currentMethod.body;
                else
                    currentClass.methods.push_back(currentMethod);
            } else {
                // Multi-line method - enter method parsing mode
                inMethod = true;
                methodNestLevel = 0;
            }
            continue;
        }

        if (std::regex_search(line, match, accessorStartPattern)) {
            currentAccessor = VBClassAccessor();
            currentAccessor.type = match[2].str();
            currentAccessor.name = match[3].str();
            currentAccessor.params = ParseParameters(match[4].str());
            currentAccessor.body = "";

            // Check for single-line accessor: Property Get Foo() : Foo = x : End Property
            // Allow trailing comments ('...) after End Property
            static std::regex singleLineEndProp(R"(:\s*(.*):\s*End\s+Property\s*('.*)?$)", std::regex::icase);
            std::smatch singleLineMatch;
            bool isSingleLine = false;

            if (std::regex_search(line, singleLineMatch, singleLineEndProp)) {
                currentAccessor.body = Trim(singleLineMatch[1].str());
                isSingleLine = true;
            }

            if (currentClass.name == "VPMLampUpdater") {
                PLOGI.printf("ScriptPatcher: VPMLampUpdater found accessor '%s %s' (singleLine=%d)", currentAccessor.type.c_str(), currentAccessor.name.c_str(), isSingleLine);
            }

            if (isSingleLine) {
                // Single-line accessor - add immediately, don't enter multi-line mode
                currentClass.accessors.push_back(currentAccessor);
            } else {
                // Multi-line accessor - enter accessor parsing mode
                inAccessor = true;
                accessorNestLevel = 0;
            }
            continue;
        }
        
        if (std::regex_search(line, match, propertyDeclPattern)) {
            bool isPublic = EqualsIgnoreCase(Trim(match[1].str()), "Public");
            std::string varList = match[2].str();
            // Strip VBScript comments (everything after ')
            size_t commentPos = varList.find('\'');
            if (commentPos != std::string::npos) {
                varList = varList.substr(0, commentPos);
            }
            std::istringstream varStream(varList);
            std::string varToken;
            while (std::getline(varStream, varToken, ',')) {
                varToken = Trim(varToken);
                bool isArray = false;
                int arraySize = -1;
                size_t parenPos = varToken.find('(');
                if (parenPos != std::string::npos) {
                    isArray = true;  // Property declared as array (e.g., "Private arr()" or "Private arr(300)")
                    // Extract array size if specified
                    size_t closeParenPos = varToken.find(')', parenPos);
                    if (closeParenPos != std::string::npos && closeParenPos > parenPos + 1) {
                        std::string sizeStr = Trim(varToken.substr(parenPos + 1, closeParenPos - parenPos - 1));
                        if (!sizeStr.empty() && std::all_of(sizeStr.begin(), sizeStr.end(), ::isdigit)) {
                            arraySize = std::stoi(sizeStr);
                        }
                    }
                    varToken = varToken.substr(0, parenPos);
                }
                varToken = Trim(varToken);
                if (!varToken.empty()) {
                    VBClassProperty prop;
                    prop.name = varToken;
                    prop.isPublic = isPublic;
                    prop.isArray = isArray;
                    prop.arraySize = arraySize;
                    currentClass.properties.push_back(prop);
                }
            }
        } else if (inClass && currentClass.name == "VPMLampUpdater") {
            // Debug: log lines that don't match any pattern in VPMLampUpdater
            PLOGI.printf("ScriptPatcher: VPMLampUpdater unmatched line: '%.60s'", trimmedLine.c_str());
        }
    }
    return classes;
}

// ============================================================================
// CLASS EMULATION - Phase 2: Code Generation
// ============================================================================

// Escape special regex characters in a string


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

std::string ScriptPatcher::TransformNewStatements(const std::string& script,
                                                   const std::vector<VBClassDefinition>& classes) {
    std::string result = script;

    // Build map of class name -> default method name
    std::unordered_map<std::string, std::string> defaultMethods;
    for (const auto& cls : classes) {
        for (const auto& method : cls.methods) {
            if (method.isDefault) {
                defaultMethods[cls.name] = method.name;
                break;
            }
        }
    }

    for (const auto& cls : classes) {
        std::string className = cls.name;
        std::string escapedClassName = EscapeRegex(className);

        // Pattern 3: Constructor with args (default method): (new ClassName)(args)
        // Transform: Set x = (new ClassName)(args) -> Set x = ClassName_defaultMethod(ClassName_Create(), args)
        auto it = defaultMethods.find(className);
        if (it != defaultMethods.end()) {
            std::string defaultMethodName = it->second;
            // Pattern: (new ClassName)(args) - with parentheses around "new ClassName"
            std::string pattern3 = "\\(\\s*new\\s+" + escapedClassName + "\\s*\\)\\s*\\(([^)]*)\\)";
            std::regex newPattern3(pattern3, std::regex::icase);
            result = std::regex_replace(result, newPattern3,
                className + "_" + defaultMethodName + "(" + className + "_Create(), $1)");
        }

        // Pattern 1: Simple variable assignment: Set varName = New ClassName
        std::string pattern1 = "(Set\\s+\\w+\\s*=\\s*)New\\s+" + escapedClassName + "\\b";
        std::regex newPattern1(pattern1, std::regex::icase);
        result = std::regex_replace(result, newPattern1, "$1" + className + "_Create()");

        // Pattern 2: Array element assignment: Set arrayName(idx) = New ClassName
        std::string pattern2 = "(Set\\s+\\w+\\s*\\([^)]+\\)\\s*=\\s*)New\\s+" + escapedClassName + "\\b";
        std::regex newPattern2(pattern2, std::regex::icase);
        result = std::regex_replace(result, newPattern2, "$1" + className + "_Create()");
    }
    return result;
}


/**
 * Transform method calls on emulated class instances.
 *
 * Converts: obj.Method(args)  ->  ClassName_Method(obj, args)   [expression context]
 *           obj.Method(args)  ->  ClassName_Method obj, args    [statement context]
 *           obj.Method args   ->  ClassName_Method obj, args    [space-separated args]
 *           obj.Method        ->  ClassName_Method obj          [no args]
 *
 * OPTIMIZATION: This uses a single-pass approach with regex_iterator instead of
 * multiple regex_replace calls. The old approach did O(V * M * 4) regex operations
 * where V = number of variables and M = number of methods. This does O(N) where
 * N = number of word.word patterns in the script.
 *
 * @param script The VBScript source after class definitions have been emulated
 * @param classes The parsed class definitions for lookup
 * @return The script with method calls transformed
 */
std::string ScriptPatcher::TransformMethodCalls(const std::string& script,
                                                 const std::vector<VBClassDefinition>& classes) {
    // =========================================================================
    // PHASE 1: Build lookup tables for O(1) access during transformation
    // =========================================================================

    // Map: className -> (lowercase method name -> original case method name)
    // This allows case-insensitive matching while preserving original case in output
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> classMethods;
    std::unordered_set<std::string> classNames;
    for (const auto& cls : classes) {
        classNames.insert(cls.name);
        std::unordered_map<std::string, std::string> methods;
        for (const auto& m : cls.methods) {
            methods[ToLower(m.name)] = m.name;  // lowercase key -> original value
        }
        classMethods[cls.name] = methods;
    }

    // =========================================================================
    // PHASE 2: Find all variables that hold emulated class instances
    // =========================================================================

    // Scan for: Set varName = ClassName_Create()
    // Build maps: lowercase var -> className, lowercase var -> original case var
    std::unordered_map<std::string, std::string> varTypes;       // lowerVar -> className
    std::unordered_map<std::string, std::string> varOriginalCase; // lowerVar -> originalVar
    for (const auto& cls : classes) {
        std::string escapedClassName = EscapeRegex(cls.name);
        std::string pattern = "Set\\s+(\\w+)\\s*=\\s*" + escapedClassName + "_Create\\(\\)";
        std::regex setPattern(pattern, std::regex::icase);
        std::smatch match;
        std::string::const_iterator searchStart(script.cbegin());
        while (std::regex_search(searchStart, script.cend(), match, setPattern)) {
            std::string varName = match[1].str();
            std::string lowerVar = ToLower(varName);
            varTypes[lowerVar] = cls.name;
            varOriginalCase[lowerVar] = varName;
            searchStart = match.suffix().first;
        }
    }

    // Early exit if no emulated class instances found
    if (varTypes.empty()) return script;

    // =========================================================================
    // PHASE 3: Single-pass transformation using regex_iterator
    // =========================================================================

    // Regex breakdown: \b(\w+)\.(\w+)(\s*\(([^)]*)\)|[ \t]+([^'=:\r\n\s][^:\r\n]*))?
    //
    // \b(\w+)          - Capture group 1: variable name (word boundary to avoid partial matches)
    // \.               - Literal dot
    // (\w+)            - Capture group 2: method name
    // (                - Capture group 3: optional args section (either parens or space-separated)
    //   \s*\(([^)]*)\) - Alternative A: parenthesized args, group 4 captures inside parens
    //   |              - OR
    //   [ \t]+         - Horizontal whitespace (NOT \s to avoid matching across newlines)
    //   ([^'=:\r\n\s]  - Capture group 5: first char must not be quote/equals/colon/newline/space
    //                    (prevents matching comments and assignment operators)
    //   [^:\r\n]*)     - Rest of args until colon or newline
    // )?               - Args section is optional (handles no-arg calls)
    //
    static std::regex dotAccess(R"(\b(\w+)\.(\w+)(\s*\(([^)]*)\)|[ \t]+([^'=:\r\n\s][^:\r\n]*))?)", std::regex::icase);

    std::string result;
    result.reserve(script.size() + script.size() / 10);  // Pre-allocate with 10% buffer

    std::sregex_iterator it(script.begin(), script.end(), dotAccess);
    std::sregex_iterator end;
    size_t lastPos = 0;

    while (it != end) {
        std::smatch match = *it;
        std::string lowerVar = ToLower(match[1].str());
        std::string lowerMember = ToLower(match[2].str());

        // Append any text between last match and this match (unchanged)
        result.append(script, lastPos, match.position() - lastPos);

        // Check if this is a known emulated class variable
        auto varIt = varTypes.find(lowerVar);
        if (varIt != varTypes.end()) {
            const std::string& className = varIt->second;
            const auto& methods = classMethods[className];
            auto methodIt = methods.find(lowerMember);

            if (methodIt != methods.end()) {
                // Found a method call on an emulated class instance - transform it
                const std::string& origMethod = methodIt->second;
                const std::string& origVar = varOriginalCase[lowerVar];

                std::string args;
                bool hasParens = false;

                if (match[3].matched) {
                    // IMPORTANT: Check match[4].matched to determine if parens were used,
                    // NOT by searching for '(' in the string. Comments like 'point# (note)'
                    // contain parentheses but should use the space-separated args path.
                    if (match[4].matched) {
                        // Parenthesized args: var.Method(args) or var.Method()
                        hasParens = true;
                        args = match[4].str();
                    } else if (match[5].matched) {
                        // Space-separated args: var.Method arg1, arg2
                        args = match[5].str();
                        // Trim trailing whitespace (but preserve args content)
                        size_t endPos = args.find_last_not_of(" \t\r\n");
                        if (endPos != std::string::npos)
                            args = args.substr(0, endPos + 1);
                        else
                            args.clear();
                    }
                }
                // If match[3] not matched: no-args call like var.Method

                // Determine output format based on expression vs statement context
                // Expression context: preceded by '=' (e.g., "x = obj.Method()")
                // Statement context: standalone call (e.g., "obj.Method arg")
                size_t checkPos = result.size();
                while (checkPos > 0 && (result[checkPos-1] == ' ' || result[checkPos-1] == '\t')) checkPos--;

                if (checkPos > 0 && result[checkPos-1] == '=') {
                    // Expression context: output as function call with parens
                    // = obj.Method(args) -> = ClassName_Method(obj, args)
                    result += className + "_" + origMethod + "(" + origVar;
                    if (!args.empty()) result += ", " + args;
                    result += ")";
                } else {
                    // Statement context: output as sub call without parens
                    // obj.Method args -> ClassName_Method obj, args
                    result += className + "_" + origMethod + " " + origVar;
                    if (!args.empty()) result += ", " + args;
                }

                lastPos = match.position() + match.length();
                ++it;
                continue;
            }
        }

        // Not a method call on emulated class - keep original text
        result += match[0].str();
        lastPos = match.position() + match.length();
        ++it;
    }

    // Append any remaining text after the last match
    result.append(script, lastPos, script.size() - lastPos);
    return result;
}






std::string ScriptPatcher::TransformPropertyAccess(const std::string& script,
                                                    const std::vector<VBClassDefinition>& classes) {
    // Collect regular props and array props separately
    std::unordered_map<std::string, std::unordered_set<std::string>> classProps;      // non-array
    std::unordered_map<std::string, std::unordered_set<std::string>> classArrayProps; // array
    for (const auto& cls : classes) {
        std::unordered_set<std::string> props;
        std::unordered_set<std::string> arrayProps;
        for (const auto& p : cls.properties) {
            if (p.isArray)
                arrayProps.insert(p.name);
            else
                props.insert(p.name);
        }
        classProps[cls.name] = props;
        classArrayProps[cls.name] = arrayProps;
    }

    std::unordered_map<std::string, std::string> varTypes;
    for (const auto& cls : classes) {
        std::string escapedClassName = EscapeRegex(cls.name);
        std::string pattern = "Set\\s+(\\w+)\\s*=\\s*" + escapedClassName + "_Create\\(\\)";
        std::regex setPattern(pattern, std::regex::icase);
        std::smatch match;
        std::string::const_iterator searchStart(script.cbegin());
        while (std::regex_search(searchStart, script.cend(), match, setPattern)) {
            varTypes[match[1].str()] = cls.name;
            searchStart = match.suffix().first;
        }
    }

    std::string result = script;
    for (const auto& [varName, className] : varTypes) {
        std::string escapedVar = EscapeRegex(varName);

        // Handle ARRAY properties: var.arrayProp(idx) -> ClassName_arrayProp(idx)
        // Array props are stored as global arrays, not in dictionary
        const auto& arrayProps = classArrayProps[className];
        for (const auto& propName : arrayProps) {
            std::string escapedProp = EscapeRegex(propName);
            // Match var.arrayProp( - array access with opening paren
            std::string ap = "\\b" + escapedVar + "\\." + escapedProp + "\\s*\\(";
            std::regex ar(ap, std::regex::icase);
            result = std::regex_replace(result, ar, className + "_" + propName + "(");
        }

        // Handle REGULAR properties: var.Prop -> var("Prop")
        const auto& props = classProps[className];
        for (const auto& propName : props) {
            std::string escapedProp = EscapeRegex(propName);
            // Use word boundary  before variable name to avoid matching substrings
            // Read: var.Prop (not = )
            std::string rp = "\\b" + escapedVar + "\\." + escapedProp + "\\b(?!\\s*=)";
            std::regex rr(rp, std::regex::icase);
            result = std::regex_replace(result, rr, varName + "(\"" + propName + "\")");
            // Write: var.Prop =
            std::string wp = "\\b" + escapedVar + "\\." + escapedProp + "\\s*=";
            std::regex wr(wp, std::regex::icase);
            result = std::regex_replace(result, wr, varName + "(\"" + propName + "\") =");
            // Set: Set var.Prop =
            std::string sp = "Set\\s+\\b" + escapedVar + "\\." + escapedProp + "\\s*=";
            std::regex sr(sp, std::regex::icase);
            result = std::regex_replace(result, sr, "Set " + varName + "(\"" + propName + "\") =");
        }
    }
    return result;
}


std::string ScriptPatcher::TransformAccessorAccess(const std::string& script,
                                                    const std::vector<VBClassDefinition>& classes) {
    // Collect accessor info: className -> {accessorName -> type (Get/Let/Set)}
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> classAccessors;
    for (const auto& cls : classes) {
        std::unordered_map<std::string, std::string> accessors;
        for (const auto& acc : cls.accessors) {
            // Store accessor type (Get, Let, Set)
            accessors[acc.name] = acc.type;
        }
        classAccessors[cls.name] = accessors;
    }

    // Build var -> className map
    std::unordered_map<std::string, std::string> varTypes;
    for (const auto& cls : classes) {
        std::string escapedClassName = EscapeRegex(cls.name);
        std::string pattern = "Set\\s+(\\w+)\\s*=\\s*" + escapedClassName + "_Create\\(\\)";
        std::regex setPattern(pattern, std::regex::icase);
        std::smatch match;
        std::string::const_iterator searchStart(script.cbegin());
        while (std::regex_search(searchStart, script.cend(), match, setPattern)) {
            varTypes[match[1].str()] = cls.name;
            searchStart = match.suffix().first;
        }
    }

    std::string result = script;
    for (const auto& [varName, className] : varTypes) {
        const auto& accessors = classAccessors[className];
        std::string escapedVar = EscapeRegex(varName);

        for (const auto& [accName, accType] : accessors) {
            std::string escapedAcc = EscapeRegex(accName);

            // Write WITH params: var.accessor(idx) = value  →  ClassName_Let_accessor var, idx, value
            // ONLY match at statement start to avoid matching comparisons in If statements
            // Statement start: line start (with optional indent), after :, after Then, after Else
            // Value capture must stop at : or Else (for single-line If statements)
            // Use balanced parentheses matching by allowing nested parens: ([^()]*(?:\([^()]*\)[^()]*)*)
            // Use word boundary \b before variable name to avoid matching substrings (e.g., Lampz in ModLampz)
            std::string wp = "(^[ \\t]*|:[ \\t]*|\\bThen\\s+|\\bElse\\s+)\\b" + escapedVar + "\\." + escapedAcc + "\\s*\\(([^()]*(?:\\([^()]*\\)[^()]*)*)\\)\\s*=\\s*([^:\\r\\n]*?)(?=\\s*(?:Else\\b|:|\\r|\\n|$))";
            std::regex wr(wp, std::regex::icase | std::regex::multiline);
            result = std::regex_replace(result, wr, "$1" + className + "_Let_" + accName + " " + varName + ", $2, $3");

            // Write WITHOUT params: var.accessor = value  →  ClassName_Let_accessor var, value
            // For Property Let with only one parameter (the value being assigned)
            // Use word boundary \b before variable name to avoid matching substrings (e.g., Lampz in ModLampz)
            std::string spw = "(^[ \\t]*|:[ \\t]*|\\bThen\\s+|\\bElse\\s+)\\b" + escapedVar + "\\." + escapedAcc + "\\s*=\\s*([^:\\r\\n]*?)(?=\\s*(?:Else\\b|:|\\r|\\n|$))";
            std::regex swr(spw, std::regex::icase | std::regex::multiline);
            result = std::regex_replace(result, swr, "$1" + className + "_Let_" + accName + " " + varName + ", $2");

            // Read WITH params: var.accessor(idx)  →  ClassName_Get_accessor(var, idx)
            // Match all remaining accessor calls (those not converted to Let above are reads)
            // Use balanced parentheses matching
            // Use word boundary \b before variable name to avoid matching substrings (e.g., Lampz in ModLampz)
            std::string rp = "\\b" + escapedVar + "\\." + escapedAcc + "\\s*\\(([^()]*(?:\\([^()]*\\)[^()]*)*)\\)";
            std::regex rr(rp, std::regex::icase);
            result = std::regex_replace(result, rr, className + "_Get_" + accName + "(" + varName + ", $1)");

            // Read WITHOUT params: var.accessor  →  ClassName_Get_accessor(var)
            // For Property Get with no parameters - must not be followed by ( or =
            // Use word boundary \b before variable name to avoid matching substrings (e.g., Lampz in ModLampz)
            std::string spr = "\\b" + escapedVar + "\\." + escapedAcc + "\\b(?!\\s*[=(])";
            std::regex srr(spr, std::regex::icase);
            result = std::regex_replace(result, srr, className + "_Get_" + accName + "(" + varName + ")");
        }
    }
    return result;
}


std::string ScriptPatcher::EmulateClasses(const std::string& script) {
    if (!HasClassDefinitions(script)) return script;

    PLOGI.printf("ScriptPatcher: Detected VBScript classes, applying emulation");
    std::vector<VBClassDefinition> classes = ParseClassDefinitions(script);
    if (classes.empty()) return script;

    // Post-parse: detect arrays from ReDim usage in class bodies
    // Properties may be declared without () but used with ReDim
    // Also detect IMPLICIT array declarations (ReDim without prior Public/Private)
    for (auto& cls : classes) {
        // Collect all method/accessor bodies plus initializeBody
        std::string allBodies = cls.initializeBody + "\n";
        for (const auto& m : cls.methods) allBodies += m.body + "\n";
        for (const auto& a : cls.accessors) allBodies += a.body + "\n";

        // Build set of existing property names (case-insensitive)
        std::unordered_set<std::string> existingProps;
        for (const auto& prop : cls.properties) {
            std::string lowerName = prop.name;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
            existingProps.insert(lowerName);
        }
        // Also add accessor names - ReDim should not override Property Let/Get/Set
        for (const auto& acc : cls.accessors) {
            std::string lowerName = acc.name;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
            existingProps.insert(lowerName);
        }

        // Check each property for ReDim usage
        for (auto& prop : cls.properties) {
            if (!prop.isArray) {
                // Look for ReDim propName( pattern
                std::string pattern = "\\bReDim\\s+" + EscapeRegex(prop.name) + "\\s*\\(";
                std::regex redimPattern(pattern, std::regex::icase);
                if (std::regex_search(allBodies, redimPattern)) {
                    prop.isArray = true;
                    PLOGI.printf("ScriptPatcher: Detected '%s' as array via ReDim usage in class '%s'",
                                prop.name.c_str(), cls.name.c_str());
                }
            }
        }

        // Find IMPLICIT array declarations: ReDim varName( where varName is not an existing property
        // These are class-level variables created via ReDim in Class_Initialize
        std::regex redimAllPattern(R"(\bReDim\s+(\w+)\s*\()", std::regex::icase);
        std::sregex_iterator it(allBodies.begin(), allBodies.end(), redimAllPattern);
        std::sregex_iterator end;
        while (it != end) {
            std::string varName = (*it)[1].str();
            std::string lowerName = varName;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

            // Skip if already a known property
            if (existingProps.find(lowerName) == existingProps.end()) {
                // Add as implicit array property
                VBClassProperty implicitProp;
                implicitProp.name = varName;
                implicitProp.isPublic = false;  // Treat as private by default
                implicitProp.isArray = true;
                implicitProp.arraySize = 0;
                cls.properties.push_back(implicitProp);
                existingProps.insert(lowerName);
                PLOGI.printf("ScriptPatcher: Added implicit array property '%s' in class '%s' (from ReDim)",
                            varName.c_str(), cls.name.c_str());
            }
            ++it;
        }
    }

    PLOGI.printf("ScriptPatcher: Found %zu classes", classes.size());

    std::unordered_set<std::string> classNames;
    for (const auto& cls : classes) {
        classNames.insert(cls.name);
        PLOGI.printf("ScriptPatcher: Emulating '%s'", cls.name.c_str());
    }

    // Handle single-line stub classes (e.g., "Class X : ... : End Class" all on one line)
    // These are skipped by ParseClassDefinitions but need to be removed and stubbed
    // First pass: just collect class names and build stub emulation (don't modify script yet)
    std::ostringstream stubEmulation;
    std::regex singleLineClassRegex(R"(^[ \t]*Class\s+(\w+)\s*:.+?End\s+Class[ \t]*$)", std::regex::icase | std::regex::multiline);
    std::smatch singleMatch;
    std::string searchStr = script;
    while (std::regex_search(searchStr, singleMatch, singleLineClassRegex)) {
        std::string className = singleMatch[1].str();
        PLOGI.printf("ScriptPatcher: Found single-line stub class '%s', creating minimal emulation", className.c_str());

        // Add to classNames for TransformNewStatements
        classNames.insert(className);

        // Create minimal stub emulation (just a factory function that returns empty Dictionary)
        stubEmulation << "' === " << className << " Stub Class Emulation ===\n";
        stubEmulation << "Function " << className << "_Create()\n";
        stubEmulation << "    Dim this_\n";
        stubEmulation << "    Set this_ = CreateObject(\"Scripting.Dictionary\")\n";
        stubEmulation << "    this_(\"__class__\") = \"" << className << "\"\n";
        stubEmulation << "    Set " << className << "_Create = this_\n";
        stubEmulation << "End Function\n\n";

        searchStr = singleMatch.suffix().str();
    }

    std::string result = script;

    // Sort by position descending
    std::sort(classes.begin(), classes.end(),
              [](const VBClassDefinition& a, const VBClassDefinition& b) { return a.startPos > b.startPos; });

    // Collect all array declarations - these need to go at the TOP of the script
    std::ostringstream arrayDecls;
    arrayDecls << "' === Class Emulation Array Declarations ===\n";
    for (const auto& cls : classes) {
        for (const auto& prop : cls.properties) {
            if (prop.isArray) {
                int size = (prop.arraySize >= 0) ? prop.arraySize : 0;
                arrayDecls << "Dim " << cls.name << "_" << prop.name << "(" << size << ")\n";
            }
        }
    }
    arrayDecls << "' === End Array Declarations ===\n\n";
    for (const auto& cls : classes) {
        std::string emulation = EmitClassEmulation(cls);
        result = result.substr(0, cls.startPos) + emulation + result.substr(cls.endPos);
    }

    // Now remove single-line stub classes from the result (after main classes are processed)
    // Using regex_replace for proper handling
    result = std::regex_replace(result, singleLineClassRegex, "' [Stub class removed - emulated below]");

    // Inject stub emulation code if any single-line classes were found
    std::string stubCode = stubEmulation.str();
    if (!stubCode.empty()) {
        arrayDecls << stubCode;
    }

    // Inject array declarations at the start (after Option Explicit if present)
    std::regex optionExplicit(R"((Option\s+Explicit[^\r\n]*[\r\n]+))", std::regex::icase);
    std::smatch match;
    if (std::regex_search(result, match, optionExplicit)) {
        result = match.prefix().str() + match[0].str() + arrayDecls.str() + match.suffix().str();
    } else {
        result = arrayDecls.str() + result;
    }

    result = TransformNewStatements(result, classes);
    result = TransformMethodCalls(result, classes);
    result = TransformPropertyAccess(result, classes);
    result = TransformAccessorAccess(result, classes);

    // Instead of runtime dispatchers (which crash Wine), use static type inference
    // to transform For Each loop bodies when we know the array element types

    // Step 1: Find all variables assigned via ClassName_Create() or ClassName_init(ClassName_Create(), ...)
    //// Pattern: Set varname = ClassName_Create() or Set varname = ClassName_init(ClassName_Create(), ...)
    std::unordered_map<std::string, std::string> varTypes; // varname -> className
    for (const auto& cls : classes) {
        // Pattern 1: Set varname = ClassName_Create()
        std::string pattern1 = "Set\\s+(\\w+)\\s*=\\s*" + cls.name + "_Create\\s*\\(";
        std::regex varAssignRegex1(pattern1, std::regex::icase);
        std::sregex_iterator it1(result.begin(), result.end(), varAssignRegex1);
        std::sregex_iterator end;
        while (it1 != end) {
            std::string varName = (*it1)[1].str();
            std::string lowerVar = varName;
            std::transform(lowerVar.begin(), lowerVar.end(), lowerVar.begin(), ::tolower);
            varTypes[lowerVar] = cls.name;
            ++it1;
        }

        // Pattern 2: Set varname = ClassName_init(ClassName_Create(), ...)
        // This handles classes with default methods (constructors with args)
        std::string pattern2 = "Set\\s+(\\w+)\\s*=\\s*" + cls.name + "_\\w+\\s*\\(\\s*" + cls.name + "_Create\\s*\\(";
        std::regex varAssignRegex2(pattern2, std::regex::icase);
        std::sregex_iterator it2(result.begin(), result.end(), varAssignRegex2);
        while (it2 != end) {
            std::string varName = (*it2)[1].str();
            std::string lowerVar = varName;
            std::transform(lowerVar.begin(), lowerVar.end(), lowerVar.begin(), ::tolower);
            varTypes[lowerVar] = cls.name;
            ++it2;
        }
    }

    PLOGI.printf("ScriptPatcher: Found %zu typed variables", varTypes.size());
    for (const auto& [var, cls] : varTypes) {
        PLOGI.printf("ScriptPatcher:   varTypes[%s] = %s", var.c_str(), cls.c_str());
    }

    // Step 2: Find For Each loops and transform method calls on loop variables
    // when the array contains known emulated class instances
    //// Pattern: For Each loopVar In Array(knownVar1, knownVar2, ...)
    // We'll transform: loopVar.method args -> ClassName_method loopVar, args

    // Build method map for quick lookup
    std::unordered_map<std::string, std::string> methodToClass; // methodName -> className
    for (const auto& cls : classes) {
        for (const auto& m : cls.methods) {
            std::string lowerMethod = m.name;
            std::transform(lowerMethod.begin(), lowerMethod.end(), lowerMethod.begin(), ::tolower);
            methodToClass[lowerMethod] = cls.name;
        }
    }

    // Find For Each loops with Array() containing known typed variables
    // For Each x In Array(LS, RS) where LS and RS are SlingshotCorrection
    std::regex forEachArrayPattern(
        R"(For\s+Each\s+(\w+)\s+In\s+Array\s*\(\s*(\w+)(?:\s*,\s*(\w+))*\s*\))",
        std::regex::icase
    );

    std::sregex_iterator forIt(result.begin(), result.end(), forEachArrayPattern);
    std::sregex_iterator forEnd;

    std::vector<std::tuple<size_t, size_t, std::string, std::string>> loopsToTransform;
    // tuple: (startPos, endPos, loopVar, className)

    while (forIt != forEnd) {
        std::string loopVar = (*forIt)[1].str();
        std::string firstArrayVar = (*forIt)[2].str();

        // Check if first array variable is a known typed variable
        std::string lowerFirstVar = firstArrayVar;
        std::transform(lowerFirstVar.begin(), lowerFirstVar.end(), lowerFirstVar.begin(), ::tolower);

        auto typeIt = varTypes.find(lowerFirstVar);
        if (typeIt != varTypes.end()) {
            std::string className = typeIt->second;
            size_t loopStart = (*forIt).position() + (*forIt).length();

            // Find the matching Next
            std::regex nextPattern("\\bNext\\b", std::regex::icase);
            std::string afterLoop = result.substr(loopStart);
            std::smatch nextMatch;
            if (std::regex_search(afterLoop, nextMatch, nextPattern)) {
                size_t loopEnd = loopStart + nextMatch.position();
                loopsToTransform.push_back({loopStart, loopEnd, loopVar, className});
            }
        }
        ++forIt;
    }

    // For each For Each loop, find the NEAREST preceding Array() assignment
    // to determine the element type (handles local variables with same name)
    std::regex forEachVarPattern(R"(For\s+Each\s+(\w+)\s+In\s+(\w+))", std::regex::icase);
    std::sregex_iterator forIt2(result.begin(), result.end(), forEachVarPattern);
    while (forIt2 != forEnd) {
        std::string loopVar = (*forIt2)[1].str();
        std::string arrayVar = (*forIt2)[2].str();
        std::string lowerArrayVar = arrayVar;
        std::transform(lowerArrayVar.begin(), lowerArrayVar.end(), lowerArrayVar.begin(), ::tolower);

        // Skip if arrayVar is "Array" (handled by previous pattern)
        if (lowerArrayVar == "array") { ++forIt2; continue; }

        size_t forEachPos = (*forIt2).position();

        // Look backwards for "arrayVar = Array(firstElem, ...)" before this For Each
        std::string beforeLoop = result.substr(0, forEachPos);
        std::regex arrayAssignPattern(arrayVar + "\\s*=\\s*Array\\s*\\(\\s*(\\w+)", std::regex::icase);
        std::smatch arrayMatch;
        std::string::const_iterator searchStart = beforeLoop.cbegin();
        std::smatch lastMatch;
        bool foundMatch = false;
        while (std::regex_search(searchStart, beforeLoop.cend(), arrayMatch, arrayAssignPattern)) {
            lastMatch = arrayMatch;
            foundMatch = true;
            searchStart = arrayMatch.suffix().first;
        }

        if (foundMatch) {
            std::string firstElem = lastMatch[1].str();
            std::string lowerFirstElem = firstElem;
            std::transform(lowerFirstElem.begin(), lowerFirstElem.end(), lowerFirstElem.begin(), ::tolower);

            auto typeIt = varTypes.find(lowerFirstElem);
            if (typeIt != varTypes.end()) {
                std::string className = typeIt->second;
                size_t loopStart = forEachPos + (*forIt2).length();

                // Find the matching Next
                std::regex nextPattern("\\bNext\\b", std::regex::icase);
                std::string afterLoop = result.substr(loopStart);
                std::smatch nextMatch;
                if (std::regex_search(afterLoop, nextMatch, nextPattern)) {
                    size_t loopEnd = loopStart + nextMatch.position();
                    loopsToTransform.push_back({loopStart, loopEnd, loopVar, className});
                    PLOGI.printf("ScriptPatcher: Found For Each %s In %s (type %s at pos %zu)",
                                loopVar.c_str(), arrayVar.c_str(), className.c_str(), forEachPos);
                }
            }
        }
        ++forIt2;
    }

    // Process loops in reverse order to preserve positions
    std::sort(loopsToTransform.begin(), loopsToTransform.end(),
              [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });

    for (const auto& [startPos, endPos, loopVar, className] : loopsToTransform) {
        std::string loopBody = result.substr(startPos, endPos - startPos);
        std::string transformedBody = loopBody;

        // Transform loopVar.method args -> ClassName_method loopVar, args
        for (const auto& cls : classes) {
            if (cls.name != className) continue;

            for (const auto& m : cls.methods) {
                std::string escapedMethod = EscapeRegex(m.name);
                std::string escapedLoopVar = EscapeRegex(loopVar);

                if (m.params.size() > 0) {
                    // Method with params: loopVar.method args
                    std::string methodPattern = "\\b" + escapedLoopVar + "\\." + escapedMethod +
                                                "\\b(?!\\s*[=(])[ \\t]+([^:\\r\\n]+?)(?=[ \\t]*(?::|\\r|\\n|$))";
                    std::regex mr(methodPattern, std::regex::icase);
                    transformedBody = std::regex_replace(transformedBody, mr,
                                                         className + "_" + m.name + " " + loopVar + ", $1");
                } else {
                    // Method without params: loopVar.method
                    std::string methodPattern = "\\b" + escapedLoopVar + "\\." + escapedMethod +
                                                "\\b(?=[ \\t]*(?::|'|\\r|\\n|$))";
                    std::regex mr(methodPattern, std::regex::icase);
                    transformedBody = std::regex_replace(transformedBody, mr,
                                                         className + "_" + m.name + " " + loopVar);
                }
            }

            // Transform property access: loopVar.prop = value -> loopVar("prop") = value
            for (const auto& prop : cls.properties) {
                if (prop.isArray) continue;
                std::string escapedProp = EscapeRegex(prop.name);
                std::string escapedLoopVar = EscapeRegex(loopVar);

                // Assignment: loopVar.prop = value
                std::string propPattern = "\\b" + escapedLoopVar + "\\." + escapedProp +
                                          "\\s*=\\s*([^:\\r\\n]+?)(?=[ \\t]*(?::|\\r|\\n|$))";
                std::regex pr(propPattern, std::regex::icase);
                transformedBody = std::regex_replace(transformedBody, pr,
                                                     loopVar + "(\"" + prop.name + "\") = $1");
            }
        }

        result = result.substr(0, startPos) + transformedBody + result.substr(endPos);
        PLOGI.printf("ScriptPatcher: Transformed For Each loop for %s (class %s)",
                     loopVar.c_str(), className.c_str());
    }

    // Step 3: Transform array element method/property access
    // When we have: Set arrayName(idx) = ClassName_Create()
    // We need to transform: arrayName(idx).method -> ClassName_method arrayName(idx)
    // And: arrayName(idx).prop -> arrayName(idx)("prop")

    // Find arrays that contain emulated class instances
    //// Pattern: Set arrayName(idx) = ClassName_Create() or ClassName_init(ClassName_Create(), ...)
    //// Also: ArrayName = Array(var1, var2, ...) where var1 is a known typed variable
    std::unordered_map<std::string, std::string> arrayElementTypes; // arrayName -> className
    for (const auto& cls : classes) {
        // Pattern 1: Set arrayName(idx) = ClassName_Create()
        std::string pattern1 = "Set\\s+(\\w+)\\s*\\([^)]+\\)\\s*=\\s*" + cls.name + "_Create\\s*\\(";
        std::regex arrayAssignRegex1(pattern1, std::regex::icase);
        std::sregex_iterator it1(result.begin(), result.end(), arrayAssignRegex1);
        std::sregex_iterator end;
        while (it1 != end) {
            std::string arrayName = (*it1)[1].str();
            std::string lowerArrayName = arrayName;
            std::transform(lowerArrayName.begin(), lowerArrayName.end(), lowerArrayName.begin(), ::tolower);
            arrayElementTypes[lowerArrayName] = cls.name;
            PLOGI.printf("ScriptPatcher: Array '%s' contains '%s' objects (pattern 1)", arrayName.c_str(), cls.name.c_str());
            ++it1;
        }

        // Pattern 2: Set arrayName(idx) = ClassName_init(ClassName_Create(), ...)
        std::string pattern2 = "Set\\s+(\\w+)\\s*\\([^)]+\\)\\s*=\\s*" + cls.name + "_\\w+\\s*\\(\\s*" + cls.name + "_Create\\s*\\(";
        std::regex arrayAssignRegex2(pattern2, std::regex::icase);
        std::sregex_iterator it2(result.begin(), result.end(), arrayAssignRegex2);
        while (it2 != end) {
            std::string arrayName = (*it2)[1].str();
            std::string lowerArrayName = arrayName;
            std::transform(lowerArrayName.begin(), lowerArrayName.end(), lowerArrayName.begin(), ::tolower);
            arrayElementTypes[lowerArrayName] = cls.name;
            PLOGI.printf("ScriptPatcher: Array '%s' contains '%s' objects (pattern 2)", arrayName.c_str(), cls.name.c_str());
            ++it2;
        }
    }

    // Pattern 3: ArrayName = Array(var1, var2, ...) where var1 is a known typed variable
    std::regex arrayLiteralPattern(R"((\w+)\s*=\s*Array\s*\(\s*(\w+))", std::regex::icase);
    std::sregex_iterator it3(result.begin(), result.end(), arrayLiteralPattern);
    std::sregex_iterator end3;
    while (it3 != end3) {
        std::string arrayName = (*it3)[1].str();
        std::string firstElem = (*it3)[2].str();
        std::string lowerArrayName = arrayName;
        std::transform(lowerArrayName.begin(), lowerArrayName.end(), lowerArrayName.begin(), ::tolower);
        std::string lowerFirstElem = firstElem;
        std::transform(lowerFirstElem.begin(), lowerFirstElem.end(), lowerFirstElem.begin(), ::tolower);

        // Check if first element is a known typed variable
        auto typeIt = varTypes.find(lowerFirstElem);
        if (typeIt != varTypes.end()) {
            arrayElementTypes[lowerArrayName] = typeIt->second;
            PLOGI.printf("ScriptPatcher: Array '%s' contains '%s' objects (pattern 3 via %s)",
                        arrayName.c_str(), typeIt->second.c_str(), firstElem.c_str());
        }
        ++it3;
    }

    PLOGI.printf("ScriptPatcher: Found %zu array element types", arrayElementTypes.size());
    for (const auto& [arr, cls] : arrayElementTypes) {
        PLOGI.printf("ScriptPatcher:   arrayElementTypes[%s] = %s", arr.c_str(), cls.c_str());
    }

    // Transform array element method calls: arrayName(idx).method args -> ClassName_method arrayName(idx), args
    // Transform array element property access: arrayName(idx).prop -> arrayName(idx)("prop")
    for (const auto& [lowerArrayName, className] : arrayElementTypes) {
        // Find the class definition
        const VBClassDefinition* classDef = nullptr;
        for (const auto& cls : classes) {
            if (cls.name == className) {
                classDef = &cls;
                break;
            }
        }
        if (!classDef) continue;

        // Transform method calls on array elements
        for (const auto& m : classDef->methods) {
            std::string escapedMethod = EscapeRegex(m.name);

            //// Pattern: arrayName(idx).method (no args, not followed by =)
            // Must match case-insensitively for array name
            // Use [^()]+ for index to avoid matching nested parens
            std::string noArgsPattern = "\\b(\\w+)\\s*\\(([^()]+)\\)\\." + escapedMethod + "\\b(?!\\s*[=(])";
            std::regex noArgsRegex(noArgsPattern, std::regex::icase);

            std::string tempResult;
            std::sregex_iterator it(result.begin(), result.end(), noArgsRegex);
            std::sregex_iterator end;
            size_t lastPos = 0;

            while (it != end) {
                std::string matchedArrayName = (*it)[1].str();
                std::string lowerMatched = matchedArrayName;
                std::transform(lowerMatched.begin(), lowerMatched.end(), lowerMatched.begin(), ::tolower);

                tempResult += result.substr(lastPos, (*it).position() - lastPos);

                if (lowerMatched == lowerArrayName) {
                    // Transform: arrayName(idx).method -> ClassName_method arrayName(idx)
                    tempResult += className + "_" + m.name + " " + matchedArrayName + "(" + (*it)[2].str() + ")";
                } else {
                    // Not our array, keep original
                    tempResult += (*it)[0].str();
                }

                lastPos = (*it).position() + (*it).length();
                ++it;
            }
            tempResult += result.substr(lastPos);
            if (!tempResult.empty()) result = tempResult;
        }

        // Build a set of property names (lowercase) for quick lookup
        std::unordered_set<std::string> propertyNames;
        std::unordered_map<std::string, std::string> propertyOriginalCase; // lowercase -> original case
        for (const auto& prop : classDef->properties) {
            if (prop.isArray) continue;
            std::string lowerProp = prop.name;
            std::transform(lowerProp.begin(), lowerProp.end(), lowerProp.begin(), ::tolower);
            propertyNames.insert(lowerProp);
            propertyOriginalCase[lowerProp] = prop.name;
        }

        // Transform ALL property access in one pass - match any .property pattern
        //// Pattern: arrayName(idx).anyProperty
        // Use [^()]+ for index to avoid matching nested parens like IsEmpty(arr(x).prop)
        std::string propPattern = "\\b(\\w+)\\s*\\(([^()]+)\\)\\.(\\w+)\\b";
        std::regex propRegex(propPattern, std::regex::icase);

        std::string tempResult;
        std::sregex_iterator it(result.begin(), result.end(), propRegex);
        std::sregex_iterator end;
        size_t lastPos = 0;

        while (it != end) {
            std::string matchedArrayName = (*it)[1].str();
            std::string indexExpr = (*it)[2].str();
            std::string propName = (*it)[3].str();

            std::string lowerMatched = matchedArrayName;
            std::transform(lowerMatched.begin(), lowerMatched.end(), lowerMatched.begin(), ::tolower);
            std::string lowerProp = propName;
            std::transform(lowerProp.begin(), lowerProp.end(), lowerProp.begin(), ::tolower);

            tempResult += result.substr(lastPos, (*it).position() - lastPos);

            // Check if this is our array AND the property is from our class
            if (lowerMatched == lowerArrayName && propertyNames.count(lowerProp) > 0) {
                // Check what follows the match to determine if it's an assignment
                size_t afterMatch = (*it).position() + (*it).length();
                std::string afterText = result.substr(afterMatch, 10);
                // Trim leading whitespace
                size_t nonWs = afterText.find_first_not_of(" \t");
                bool isAssignment = (nonWs != std::string::npos && afterText[nonWs] == '=');

                // Transform: arrayName(idx).prop -> arrayName(idx)("prop")
                // Use original case from class definition for the property name
                std::string origProp = propertyOriginalCase[lowerProp];
                tempResult += matchedArrayName + "(" + indexExpr + ")(\"" + origProp + "\")";
                PLOGI.printf("ScriptPatcher: Transformed %s(%s).%s to dictionary access",
                            matchedArrayName.c_str(), indexExpr.c_str(), propName.c_str());
            } else {
                // Not our array or not a class property, keep original
                tempResult += (*it)[0].str();
            }

            lastPos = (*it).position() + (*it).length();
            ++it;
        }
        tempResult += result.substr(lastPos);
        if (!tempResult.empty()) result = tempResult;

        // Transform accessor calls on array elements (Property Let/Get)
        // IMPORTANT: Process Let/Set BEFORE Get to avoid Get matching assignment targets
        // First pass: Let/Set accessors
        for (const auto& acc : classDef->accessors) {
            if (!EqualsIgnoreCase(acc.type, "Let") && !EqualsIgnoreCase(acc.type, "Set")) continue;
            std::string escapedAcc = EscapeRegex(acc.name);

            {
                // Property Let: arrayName(idx).accessor = value -> ClassName_Let_accessor arrayName(idx), value
                // Use [^()]+ for index to avoid matching nested parens
                std::string letPattern = "(^[ \\t]*|:[ \\t]*|\\bThen[ \\t]+|\\bElse[ \\t]+)(\\w+)\\s*\\(([^()]+)\\)\\." + escapedAcc + "\\s*=\\s*([^:\\r\\n]+?)(?=[ \\t]*(?::|\\r|\\n|$))";
                std::regex letRegex(letPattern, std::regex::icase | std::regex::multiline);

                std::string tempResult;
                std::sregex_iterator it(result.begin(), result.end(), letRegex);
                std::sregex_iterator end;
                size_t lastPos = 0;

                while (it != end) {
                    std::string matchedArrayName = (*it)[2].str();
                    std::string lowerMatched = matchedArrayName;
                    std::transform(lowerMatched.begin(), lowerMatched.end(), lowerMatched.begin(), ::tolower);

                    tempResult += result.substr(lastPos, (*it).position() - lastPos);

                    if (lowerMatched == lowerArrayName) {
                        tempResult += (*it)[1].str() + className + "_Let_" + acc.name + " " + matchedArrayName + "(" + (*it)[3].str() + "), " + (*it)[4].str();
                    } else {
                        tempResult += (*it)[0].str();
                    }

                    lastPos = (*it).position() + (*it).length();
                    ++it;
                }
                tempResult += result.substr(lastPos);
                if (!tempResult.empty()) result = tempResult;
            }

        }

        // Second pass: Get accessors (AFTER all Let/Set have been processed)
        for (const auto& acc : classDef->accessors) {
            if (!EqualsIgnoreCase(acc.type, "Get")) continue;
            std::string escapedAcc = EscapeRegex(acc.name);

            // Property Get: arrayName(idx).accessor -> ClassName_Get_accessor(arrayName(idx))
            // Don't exclude "= " - the Let pattern already handled assignments at statement boundaries,
            // so remaining "arr(i).prop = value" are comparisons (like "If arr(i).prop = x Then")
            std::string getPattern = "\\b(\\w+)\\s*\\(([^()]+)\\)\\." + escapedAcc + "\\b";
            std::regex getRegex(getPattern, std::regex::icase);

            std::string tempResult;
            std::sregex_iterator it(result.begin(), result.end(), getRegex);
            std::sregex_iterator end;
            size_t lastPos = 0;

            while (it != end) {
                std::string matchedArrayName = (*it)[1].str();
                std::string indexExpr = (*it)[2].str();
                std::string lowerMatched = matchedArrayName;
                std::transform(lowerMatched.begin(), lowerMatched.end(), lowerMatched.begin(), ::tolower);

                tempResult += result.substr(lastPos, (*it).position() - lastPos);

                if (lowerMatched == lowerArrayName) {
                    // Transform: arrayName(idx).accessor -> ClassName_Get_accessor(arrayName(idx))
                    tempResult += className + "_Get_" + acc.name + "(" + matchedArrayName + "(" + indexExpr + "))";
                    PLOGI.printf("ScriptPatcher: Transformed %s(%s).%s to getter call",
                                matchedArrayName.c_str(), indexExpr.c_str(), acc.name.c_str());
                } else {
                    tempResult += (*it)[0].str();
                }

                lastPos = (*it).position() + (*it).length();
                ++it;
            }
            tempResult += result.substr(lastPos);
            if (!tempResult.empty()) result = tempResult;
        }
    }

    // Step 4: Transform ExecuteGlobal string templates that contain dot notation
    // The FlipperPolarity class uses ExecuteGlobal to create dynamic event handlers:
    // str = "Sub " & aTrigger.name & "_Hit() : " & aName & ".AddBall ActiveBall : End Sub'"
    // This needs to be transformed to use the emulated method call syntax.
    for (const auto& cls : classes) {
        for (const auto& m : cls.methods) {
            //// Pattern: & aName & ".methodName args
            // Transform to: FlipperPolarity_methodName " & aName & ", args
            std::string dotPattern = R"(\"\s*&\s*(\w+)\s*&\s*\"\.)" + EscapeRegex(m.name) + R"(\s+([^"]+))";
            std::regex dotRegex(dotPattern, std::regex::icase);

            std::string tempResult;
            std::sregex_iterator it(result.begin(), result.end(), dotRegex);
            std::sregex_iterator end;
            size_t lastPos = 0;

            while (it != end) {
                std::string varName = (*it)[1].str();
                std::string args = (*it)[2].str();

                tempResult += result.substr(lastPos, (*it).position() - lastPos);
                // Transform to: ClassName_methodName " & varName & ", args
                tempResult += cls.name + "_" + m.name + " \" & " + varName + " & \", " + args;

                lastPos = (*it).position() + (*it).length();
                ++it;
            }
            tempResult += result.substr(lastPos);
            if (!tempResult.empty() && tempResult != result) {
                result = tempResult;
                PLOGI.printf("ScriptPatcher: Transformed ExecuteGlobal template for %s.%s",
                            cls.name.c_str(), m.name.c_str());
            }
        }
    }

    return result;
}

// ============================================================================
// EXISTING PATCHES
// ============================================================================


#endif // __STANDALONE__
