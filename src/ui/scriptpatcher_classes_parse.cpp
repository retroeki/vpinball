/**
 * @file scriptpatcher_classes_parse.cpp
 * @brief VBScript Class Definition Parsing
 *
 * Parses VBScript Class definitions to extract:
 * - Class names and boundaries
 * - Properties (including arrays with sizes)
 * - Methods (Sub/Function with parameters)
 * - Accessors (Property Get/Let/Set)
 * - Class_Initialize and Class_Terminate bodies
 */

#include "stdafx.h"

#ifdef __STANDALONE__

#include "scriptpatcher.h"
#include "scriptpatcher_internal.h"
#include <sstream>
#include <algorithm>

// ============================================================================
// CLASS EMULATION - Phase 1: Parsing
// ============================================================================

bool ScriptPatcher::HasClassDefinitions(const std::string& script) {
    static const RE2 classPattern(R"((?i)\bClass\s+\w+)");
    return RE2Search(script, classPattern);
}


std::vector<std::string> ScriptPatcher::ParseParameters(const std::string& paramStr) {
    std::vector<std::string> params;
    std::string trimmed = Trim(paramStr);
    if (trimmed.empty()) return params;

    std::istringstream stream(trimmed);
    std::string token;
    static const RE2 byvalRef(R"((?i)^(ByVal|ByRef)\s+)");
    while (std::getline(stream, token, ',')) {
        token = Trim(token);
        token = RE2Replace(token, byvalRef, "");
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

    static const RE2 classStartPattern(R"((?i)^\s*Class\s+(\w+))");
    static const RE2 classEndPattern(R"((?i)^\s*End\s+Class\s*$)");
    // RE2 doesn't support negative lookahead, so we match broadly and filter in code
    static const RE2 propertyDeclPattern(R"((?i)^\s*(Public|Private)\s+(.+)$)");
    // Dim declarations at class level are private members (must be handled separately from method-level Dim)
    static const RE2 dimDeclPattern(R"((?i)^\s*Dim\s+(.+)$)");
    static const RE2 methodStartPattern(R"((?i)^\s*(Public\s+|Private\s+)?(Default\s+)?(Sub|Function)\s+(\w+)(?:\s*\(([^)]*)\))?)");
    static const RE2 methodEndSubPattern(R"((?i)^\s*End\s+Sub\s*$)");
    static const RE2 methodEndFuncPattern(R"((?i)^\s*End\s+Function\s*$)");
    static const RE2 accessorStartPattern(R"((?i)^\s*(Public\s+|Private\s+)?Property\s+(Get|Let|Set)\s+(\w+)(?:\s*\(([^)]*)\))?)");
    static const RE2 accessorEndPattern(R"((?i)^\s*End\s+Property\s*$)");
    // Nest patterns - match at line start (with whitespace) OR after statement separator (:)
    // For inline blocks like "Dim x : For x = 0 to N"
    // Use ^\s* to allow leading whitespace at line start
    static const RE2 nestStartPattern(R"((?i)(^\s*|:\s*)(If\s+.*\s+Then\s*$|For\s+|Do\s+|While\s+|Select\s+Case))");
    static const RE2 nestEndPattern(R"((?i)(^\s*|:\s*)(End\s+If|Next|Loop|Wend|End\s+Select))");

    while (std::getline(stream, line)) {
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

        std::string cap1, cap2, cap3, cap4, cap5;

        if (!inClass && RE2::PartialMatch(line, classStartPattern, &cap1)) {
            // Check for single-line class definition (Class X : ... : End Class on same line)
            static const RE2 singleLineClassPattern(R"((?i)\bEnd\s+Class\b)");
            if (RE2Search(line, singleLineClassPattern)) {
                // Single-line stub class - skip it entirely (don't emulate)
                PLOGI.printf("ScriptPatcher: Skipping single-line stub class '%s'", cap1.c_str());
                continue;
            }
            inClass = true;
            inMethod = false;  // Reset method state when entering new class
            inAccessor = false;  // Reset accessor state when entering new class
            methodNestLevel = 0;
            accessorNestLevel = 0;
            currentClass = VBClassDefinition();
            currentClass.name = cap1;
            currentClass.startPos = lineStart;
            PLOGI.printf("ScriptPatcher: Parsing class '%s'", currentClass.name.c_str());
            continue;
        }

        if (inClass && RE2Search(line, classEndPattern)) {
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
            bool isEndSub = RE2Search(line, methodEndSubPattern);
            bool isEndFunc = RE2Search(line, methodEndFuncPattern);
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
            if (RE2Search(line, nestStartPattern)) methodNestLevel++;
            if (RE2Search(line, nestEndPattern) && methodNestLevel > 0) methodNestLevel--;
            currentMethod.body += line + "\n";
            continue;
        }

        if (inAccessor) {
            if (RE2Search(line, accessorEndPattern)) {
                if (accessorNestLevel == 0) {
                    currentClass.accessors.push_back(currentAccessor);
                    inAccessor = false;
                    continue;
                }
            }
            if (RE2Search(line, nestStartPattern)) accessorNestLevel++;
            if (RE2Search(line, nestEndPattern) && accessorNestLevel > 0) accessorNestLevel--;
            currentAccessor.body += line + "\n";
            continue;
        }

        if (RE2::PartialMatch(line, methodStartPattern, &cap1, &cap2, &cap3, &cap4, &cap5)) {
            currentMethod = VBClassMethod();
            std::string visibility = cap1;
            currentMethod.isPublic = visibility.empty() || visibility.find("Public") != std::string::npos;
            currentMethod.isDefault = !cap2.empty();
            currentMethod.isFunction = EqualsIgnoreCase(Trim(cap3), "Function");
            currentMethod.name = cap4;
            currentMethod.params = ParseParameters(cap5);
            currentMethod.body = "";

            // Check for single-line method: Sub Foo() : body : End Sub
            // Pattern to find : ... : End Sub or : ... : End Function on same line
            // Allow trailing comments ('...) after End Sub/Function
            static const RE2 singleLineEndSub(R"((?i):\s*(.*):\s*End\s+Sub\s*('.*)?$)");
            static const RE2 singleLineEndFunc(R"((?i):\s*(.*):\s*End\s+Function\s*('.*)?$)");
            std::string singleLineBody, singleLineComment;
            bool isSingleLine = false;

            if (!currentMethod.isFunction && RE2::PartialMatch(line, singleLineEndSub, &singleLineBody, &singleLineComment)) {
                currentMethod.body = Trim(singleLineBody);
                isSingleLine = true;
            } else if (currentMethod.isFunction && RE2::PartialMatch(line, singleLineEndFunc, &singleLineBody, &singleLineComment)) {
                currentMethod.body = Trim(singleLineBody);
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

        if (RE2::PartialMatch(line, accessorStartPattern, &cap1, &cap2, &cap3, &cap4)) {
            currentAccessor = VBClassAccessor();
            currentAccessor.type = cap2;
            currentAccessor.name = cap3;
            currentAccessor.params = ParseParameters(cap4);
            currentAccessor.body = "";

            // Check for single-line accessor: Property Get Foo() : Foo = x : End Property
            // Allow trailing comments ('...) after End Property
            static const RE2 singleLineEndProp(R"((?i):\s*(.*):\s*End\s+Property\s*('.*)?$)");
            std::string singleLineBody, singleLineComment;
            bool isSingleLine = false;

            if (RE2::PartialMatch(line, singleLineEndProp, &singleLineBody, &singleLineComment)) {
                currentAccessor.body = Trim(singleLineBody);
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

        if (RE2::PartialMatch(line, propertyDeclPattern, &cap1, &cap2)) {
            // Filter out Sub, Function, Property, Default declarations (RE2 doesn't support (?!))
            std::string rest = Trim(cap2);
            std::string restLower = rest;
            std::transform(restLower.begin(), restLower.end(), restLower.begin(), ::tolower);
            if (restLower.rfind("sub ", 0) == 0 || restLower.rfind("sub\t", 0) == 0 ||
                restLower.rfind("function ", 0) == 0 || restLower.rfind("function\t", 0) == 0 ||
                restLower.rfind("property ", 0) == 0 || restLower.rfind("property\t", 0) == 0 ||
                restLower.rfind("default ", 0) == 0 || restLower.rfind("default\t", 0) == 0) {
                continue;  // Skip - this is a method/property accessor declaration, not a variable
            }
            bool isPublic = EqualsIgnoreCase(Trim(cap1), "Public");
            std::string varList = cap2;
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
        } else if (RE2::PartialMatch(line, dimDeclPattern, &cap1)) {
            // Dim at class level (not inside method/accessor) = private member
            // This only triggers when inMethod=false and inAccessor=false (due to earlier continues)
            std::string varList = cap1;
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
                    isArray = true;
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
                    prop.isPublic = false;  // Dim = private
                    prop.isArray = isArray;
                    prop.arraySize = arraySize;
                    currentClass.properties.push_back(prop);
                    PLOGI.printf("ScriptPatcher: Class '%s' Dim property '%s' (array=%d, size=%d)",
                                currentClass.name.c_str(), prop.name.c_str(), isArray, arraySize);
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

#endif // __STANDALONE__
