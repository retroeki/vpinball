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

#endif // __STANDALONE__
