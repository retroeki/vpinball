#include "stdafx.h"

#ifdef __STANDALONE__

#include "scriptpatcher.h"
#include <regex>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <unordered_set>

// ============================================================================
// DropTarget and StandupTarget class definitions (for DTArray/STArray patches)
// ============================================================================

const char* ScriptPatcher::DROP_TARGET_CLASS = R"(
Class DropTarget
  Private m_primary, m_secondary, m_prim, m_sw, m_animate, m_isDropped
  Public Property Get Primary(): Set Primary = m_primary: End Property
  Public Property Let Primary(input): Set m_primary = input: End Property
  Public Property Get Secondary(): Set Secondary = m_secondary: End Property
  Public Property Let Secondary(input): Set m_secondary = input: End Property
  Public Property Get Prim(): Set Prim = m_prim: End Property
  Public Property Let Prim(input): Set m_prim = input: End Property
  Public Property Get Sw(): Sw = m_sw: End Property
  Public Property Let Sw(input): m_sw = input: End Property
  Public Property Get Animate(): Animate = m_animate: End Property
  Public Property Let Animate(input): m_animate = input: End Property
  Public Property Get IsDropped(): IsDropped = m_isDropped: End Property
  Public Property Let IsDropped(input): m_isDropped = input: End Property
  Public default Function init(primary, secondary, prim, sw, animate, isDropped)
    Set m_primary = primary
    Set m_secondary = secondary
    Set m_prim = prim
    m_sw = sw
    m_animate = animate
    m_isDropped = isDropped
    Set Init = Me
  End Function
End Class
)";

const char* ScriptPatcher::STANDUP_TARGET_CLASS = R"(
Class StandupTarget
  Private m_primary, m_prim, m_sw, m_animate, m_target
  Public Property Get Primary(): Set Primary = m_primary: End Property
  Public Property Let Primary(input): Set m_primary = input: End Property
  Public Property Get Prim(): Set Prim = m_prim: End Property
  Public Property Let Prim(input): Set m_prim = input: End Property
  Public Property Get Sw(): Sw = m_sw: End Property
  Public Property Let Sw(input): m_sw = input: End Property
  Public Property Get Animate(): Animate = m_animate: End Property
  Public Property Let Animate(input): m_animate = input: End Property
  Public Property Get Target(): Target = m_target: End Property
  Public Property Let Target(input): m_target = input: End Property
  Public default Function init(primary, prim, sw, animate, target)
    Set m_primary = primary
    Set m_prim = prim
    m_sw = sw
    m_animate = animate
    m_target = target
    Set Init = Me
  End Function
End Class
)";

// Utility: Trim whitespace
static std::string Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

// Utility: Case-insensitive compare
static bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (std::tolower(a[i]) != std::tolower(b[i])) return false;
    return true;
}
// ============================================================================
// CLASS EMULATION - Phase 1: Parsing
// ============================================================================

bool ScriptPatcher::HasClassDefinitions(const std::string& script) {
    std::regex classPattern(R"(\bClass\s+\w+)", std::regex::icase);
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
        std::regex byvalRef(R"(^(ByVal|ByRef)\s+)", std::regex::icase);
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
    
    std::regex classStartPattern(R"(^\s*Class\s+(\w+))", std::regex::icase);
    std::regex classEndPattern(R"(^\s*End\s+Class\s*$)", std::regex::icase);
    std::regex propertyDeclPattern(R"(^\s*(Public|Private)\s+(?!Sub|Function|Property|Default)(.+)$)", std::regex::icase);
    std::regex methodStartPattern(R"(^\s*(Public\s+|Private\s+)?(Default\s+)?(Sub|Function)\s+(\w+)(?:\s*\(([^)]*)\))?)", std::regex::icase);
    std::regex methodEndSubPattern(R"(^\s*End\s+Sub\s*$)", std::regex::icase);
    std::regex methodEndFuncPattern(R"(^\s*End\s+Function\s*$)", std::regex::icase);
    std::regex accessorStartPattern(R"(^\s*(Public\s+|Private\s+)?Property\s+(Get|Let|Set)\s+(\w+)(?:\s*\(([^)]*)\))?)", std::regex::icase);
    std::regex accessorEndPattern(R"(^\s*End\s+Property\s*$)", std::regex::icase);
    // Nest patterns - match at line start (with whitespace) OR after statement separator (:)
    // For inline blocks like "Dim x : For x = 0 to N"
    // Use ^\s* to allow leading whitespace at line start
    std::regex nestStartPattern(R"((^\s*|:\s*)(If\s+.*\s+Then\s*$|For\s+|Do\s+|While\s+|Select\s+Case))", std::regex::icase);
    std::regex nestEndPattern(R"((^\s*|:\s*)(End\s+If|Next|Loop|Wend|End\s+Select))", std::regex::icase);

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
            std::regex singleLineEndSub(R"(:\s*(.*):\s*End\s+Sub\s*('.*)?$)", std::regex::icase);
            std::regex singleLineEndFunc(R"(:\s*(.*):\s*End\s+Function\s*('.*)?$)", std::regex::icase);
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
            std::regex singleLineEndProp(R"(:\s*(.*):\s*End\s+Property\s*('.*)?$)", std::regex::icase);
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
static std::string EscapeRegex(const std::string& str) {
    static const std::regex specialChars(R"([-[\]{}()*+?.,\\^$|#\s])");
    return std::regex_replace(str, specialChars, "\\$&");
}

std::string ScriptPatcher::TransformMethodBody(const std::string& body, const VBClassDefinition& classDef) {
    std::string result = body;
    // Me.Property -> this_("Property")
    std::regex meDotPattern(R"(\bMe\.(\w+))", std::regex::icase);
    result = std::regex_replace(result, meDotPattern, "this_(\"$1\")");
    // Standalone Me -> this_
    std::regex mePattern(R"(\bMe\b(?!\.))", std::regex::icase);
    result = std::regex_replace(result, mePattern, "this_");

    for (const auto& prop : classDef.properties) {
        std::string escapedName = EscapeRegex(prop.name);

        if (prop.isArray) {
            // Array properties: replace ALL occurrences with global variable name
            // e.g., ballvel -> CoRTracker_ballvel (used as global)
            std::string globalName = classDef.name + "_" + prop.name;

            // First handle object.PropName pattern -> just global name (remove object reference)
            // e.g., aObj.ModIn -> Dampener_ModIn
            std::string objPropPattern = "\\b\\w+\\." + escapedName + "\\b";
            std::regex objPropRegex(objPropPattern, std::regex::icase);
            result = std::regex_replace(result, objPropRegex, globalName);

            // Then replace standalone PropName -> ClassName_PropName
            std::string propPattern = "\\b" + escapedName + "\\b";
            std::regex allPattern(propPattern, std::regex::icase);
            result = std::regex_replace(result, allPattern, globalName);
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
            // Pattern: accessor(params) = value
            std::string letPattern = "\\b" + escapedName + "\\s*\\(([^)]+)\\)\\s*=\\s*(.+)";
            std::regex letRegex(letPattern, std::regex::icase);
            result = std::regex_replace(result, letRegex,
                classDef.name + "_" + accessor.type + "_" + accessor.name + " this_, $1, $2");
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
        std::string transformedBody = TransformMethodBody(method.body, classDef);

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
            std::string tb = TransformMethodBody(accessor.body, classDef);
            // Transform return value: accessorName = ... -> FunctionName = ...
            // In VBScript Property Get, you assign to the property name to return
            // Handle both "accessorName =" and "Set accessorName ="
            std::string escapedAccessorName = EscapeRegex(accessor.name);
            std::regex returnPattern("(^|:|\\s)" + escapedAccessorName + "\\s*=", std::regex::icase);
            std::regex setReturnPattern("(^|:|\\s)(Set\\s+)" + escapedAccessorName + "\\s*=", std::regex::icase);
            tb = std::regex_replace(tb, setReturnPattern, "$1$2" + funcName + " =");
            tb = std::regex_replace(tb, returnPattern, "$1" + funcName + " =");
            std::istringstream s(tb); std::string l;
            while (std::getline(s, l)) out << "    " << l << "\n";
            out << "End Function\n\n";
        } else {
            out << "Sub " << classDef.name << "_" << accessor.type << "_" << accessor.name << "(" << paramList << ")\n";
            std::string tb = TransformMethodBody(accessor.body, classDef);
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
                                                   const std::unordered_set<std::string>& classNames) {
    std::string result = script;
    for (const auto& className : classNames) {
        std::string escapedClassName = EscapeRegex(className);
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

std::string ScriptPatcher::TransformMethodCalls(const std::string& script,
                                                 const std::vector<VBClassDefinition>& classes) {
    // Build method lookup
    std::unordered_map<std::string, std::unordered_set<std::string>> classMethods;
    for (const auto& cls : classes) {
        std::unordered_set<std::string> methods;
        for (const auto& m : cls.methods) methods.insert(m.name);
        classMethods[cls.name] = methods;
    }

    // Track variable types
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
        const auto& methods = classMethods[className];
        std::string escapedVar = EscapeRegex(varName);
        for (const auto& methodName : methods) {
            std::string escapedMethod = EscapeRegex(methodName);
            // var.Method(args)
            std::string wp = escapedVar + "\\." + escapedMethod + "\\s*\\(([^)]*)\\)";
            std::regex wpr(wp, std::regex::icase);
            result = std::regex_replace(result, wpr, className + "_" + methodName + " " + varName + ", $1");
            // var.Method args (but NOT if followed only by a comment)
            // First char of args must NOT be quote or whitespace to prevent matching comments
            // Use [ \t]+ instead of \s+ to avoid matching across newlines
            std::string np = escapedVar + "\\." + escapedMethod + "[ \\t]+([^'\\s:\\r\\n][^:\\r\\n]*)";
            std::regex npr(np, std::regex::icase);
            result = std::regex_replace(result, npr, className + "_" + methodName + " " + varName + ", $1");
            // var.Method (no args)
            std::string na = escapedVar + "\\." + escapedMethod + "\\b(?!\\s*[=(])";
            std::regex nar(na, std::regex::icase);
            result = std::regex_replace(result, nar, className + "_" + methodName + " " + varName);
        }
    }
    return result;
}

std::string ScriptPatcher::TransformPropertyAccess(const std::string& script,
                                                    const std::vector<VBClassDefinition>& classes) {
    std::unordered_map<std::string, std::unordered_set<std::string>> classProps;
    for (const auto& cls : classes) {
        std::unordered_set<std::string> props;
        for (const auto& p : cls.properties) props.insert(p.name);
        classProps[cls.name] = props;
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
        const auto& props = classProps[className];
        std::string escapedVar = EscapeRegex(varName);
        for (const auto& propName : props) {
            std::string escapedProp = EscapeRegex(propName);
            // Read: var.Prop (not = )
            std::string rp = escapedVar + "\\." + escapedProp + "\\b(?!\\s*=)";
            std::regex rr(rp, std::regex::icase);
            result = std::regex_replace(result, rr, varName + "(\"" + propName + "\")");
            // Write: var.Prop =
            std::string wp = escapedVar + "\\." + escapedProp + "\\s*=";
            std::regex wr(wp, std::regex::icase);
            result = std::regex_replace(result, wr, varName + "(\"" + propName + "\") =");
            // Set: Set var.Prop =
            std::string sp = "Set\\s+" + escapedVar + "\\." + escapedProp + "\\s*=";
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
            std::string wp = "(^[ \\t]*|:[ \\t]*|\\bThen\\s+|\\bElse\\s+)" + escapedVar + "\\." + escapedAcc + "\\s*\\(([^()]*(?:\\([^()]*\\)[^()]*)*)\\)\\s*=\\s*([^:\\r\\n]*?)(?=\\s*(?:Else\\b|:|\\r|\\n|$))";
            std::regex wr(wp, std::regex::icase | std::regex::multiline);
            result = std::regex_replace(result, wr, "$1" + className + "_Let_" + accName + " " + varName + ", $2, $3");

            // Write WITHOUT params: var.accessor = value  →  ClassName_Let_accessor var, value
            // For Property Let with only one parameter (the value being assigned)
            std::string spw = "(^[ \\t]*|:[ \\t]*|\\bThen\\s+|\\bElse\\s+)" + escapedVar + "\\." + escapedAcc + "\\s*=\\s*([^:\\r\\n]*?)(?=\\s*(?:Else\\b|:|\\r|\\n|$))";
            std::regex swr(spw, std::regex::icase | std::regex::multiline);
            result = std::regex_replace(result, swr, "$1" + className + "_Let_" + accName + " " + varName + ", $2");

            // Read WITH params: var.accessor(idx)  →  ClassName_Get_accessor(var, idx)
            // Match all remaining accessor calls (those not converted to Let above are reads)
            // Use balanced parentheses matching
            std::string rp = escapedVar + "\\." + escapedAcc + "\\s*\\(([^()]*(?:\\([^()]*\\)[^()]*)*)\\)";
            std::regex rr(rp, std::regex::icase);
            result = std::regex_replace(result, rr, className + "_Get_" + accName + "(" + varName + ", $1)");

            // Read WITHOUT params: var.accessor  →  ClassName_Get_accessor(var)
            // For Property Get with no parameters - must not be followed by ( or =
            std::string spr = escapedVar + "\\." + escapedAcc + "\\b(?!\\s*[=(])";
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

    std::string result = script;
    for (const auto& cls : classes) {
        std::string emulation = EmitClassEmulation(cls);
        result = result.substr(0, cls.startPos) + emulation + result.substr(cls.endPos);
    }

    // Inject array declarations at the start (after Option Explicit if present)
    std::regex optionExplicit(R"((Option\s+Explicit[^\r\n]*[\r\n]+))", std::regex::icase);
    std::smatch match;
    if (std::regex_search(result, match, optionExplicit)) {
        result = match.prefix().str() + match[0].str() + arrayDecls.str() + match.suffix().str();
    } else {
        result = arrayDecls.str() + result;
    }

    result = TransformNewStatements(result, classNames);
    result = TransformMethodCalls(result, classes);
    result = TransformPropertyAccess(result, classes);
    result = TransformAccessorAccess(result, classes);

    // Instead of runtime dispatchers (which crash Wine), use static type inference
    // to transform For Each loop bodies when we know the array element types

    // Step 1: Find all variables assigned via ClassName_Create()
    // Pattern: Set varname = ClassName_Create()
    std::unordered_map<std::string, std::string> varTypes; // varname -> className
    for (const auto& cls : classes) {
        std::string pattern = "Set\\s+(\\w+)\\s*=\\s*" + cls.name + "_Create\\s*\\(";
        std::regex varAssignRegex(pattern, std::regex::icase);
        std::sregex_iterator it(result.begin(), result.end(), varAssignRegex);
        std::sregex_iterator end;
        while (it != end) {
            std::string varName = (*it)[1].str();
            // Convert to lowercase for case-insensitive matching
            std::string lowerVar = varName;
            std::transform(lowerVar.begin(), lowerVar.end(), lowerVar.begin(), ::tolower);
            varTypes[lowerVar] = cls.name;
            ++it;
        }
    }

    PLOGI.printf("ScriptPatcher: Found %zu typed variables", varTypes.size());

    // Step 2: Find For Each loops and transform method calls on loop variables
    // when the array contains known emulated class instances
    // Pattern: For Each loopVar In Array(knownVar1, knownVar2, ...)
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
    // Pattern: Set arrayName(idx) = ClassName_Create()
    std::unordered_map<std::string, std::string> arrayElementTypes; // arrayName -> className
    for (const auto& cls : classes) {
        std::string pattern = "Set\\s+(\\w+)\\s*\\([^)]+\\)\\s*=\\s*" + cls.name + "_Create\\s*\\(";
        std::regex arrayAssignRegex(pattern, std::regex::icase);
        std::sregex_iterator it(result.begin(), result.end(), arrayAssignRegex);
        std::sregex_iterator end;
        while (it != end) {
            std::string arrayName = (*it)[1].str();
            std::string lowerArrayName = arrayName;
            std::transform(lowerArrayName.begin(), lowerArrayName.end(), lowerArrayName.begin(), ::tolower);
            arrayElementTypes[lowerArrayName] = cls.name;
            PLOGI.printf("ScriptPatcher: Array '%s' contains '%s' objects", arrayName.c_str(), cls.name.c_str());
            ++it;
        }
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

            // Pattern: arrayName(idx).method (no args, not followed by =)
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
        // Pattern: arrayName(idx).anyProperty
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
        for (const auto& acc : classDef->accessors) {
            std::string escapedAcc = EscapeRegex(acc.name);

            if (EqualsIgnoreCase(acc.type, "Let") || EqualsIgnoreCase(acc.type, "Set")) {
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
    }

    // Step 4: Transform ExecuteGlobal string templates that contain dot notation
    // The FlipperPolarity class uses ExecuteGlobal to create dynamic event handlers:
    // str = "Sub " & aTrigger.name & "_Hit() : " & aName & ".AddBall ActiveBall : End Sub'"
    // This needs to be transformed to use the emulated method call syntax.
    for (const auto& cls : classes) {
        for (const auto& m : cls.methods) {
            // Pattern: & aName & ".methodName args
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

bool ScriptPatcher::UsesDTArray(const std::string& script) {
    std::regex p(R"(DTArray\s*\(\s*\w+\s*\)\s*\(\s*\d+\s*\))", std::regex::icase);
    return std::regex_search(script, p);
}

bool ScriptPatcher::UsesSTArray(const std::string& script) {
    std::regex p(R"(STArray\s*\(\s*\w+\s*\)\s*\(\s*\d+\s*\))", std::regex::icase);
    return std::regex_search(script, p);
}

std::string ScriptPatcher::InjectDropTargetClass(const std::string& script) {
    std::regex existing(R"(Class\s+DropTarget)", std::regex::icase);
    if (std::regex_search(script, existing)) return script;
    std::regex firstDef(R"((\r?\n)([ \t]*)(DT\d+\s*=\s*Array\s*\())", std::regex::icase);
    std::smatch m;
    if (std::regex_search(script, m, firstDef))
        return script.substr(0, m.position()) + "\n" + DROP_TARGET_CLASS + script.substr(m.position());
    std::regex optExp(R"(^\s*Option\s+Explicit\s*$)", std::regex::icase | std::regex::multiline);
    if (std::regex_search(script, m, optExp))
        return script.substr(0, m.position() + m.length()) + "\n" + DROP_TARGET_CLASS + script.substr(m.position() + m.length());
    return std::string(DROP_TARGET_CLASS) + script;
}

std::string ScriptPatcher::InjectStandupTargetClass(const std::string& script) {
    std::regex existing(R"(Class\s+StandupTarget)", std::regex::icase);
    if (std::regex_search(script, existing)) return script;
    std::regex firstDef(R"((\r?\n)([ \t]*)(ST\d+\s*=\s*Array\s*\())", std::regex::icase);
    std::smatch m;
    if (std::regex_search(script, m, firstDef))
        return script.substr(0, m.position()) + "\n" + STANDUP_TARGET_CLASS + script.substr(m.position());
    std::regex optExp(R"(^\s*Option\s+Explicit\s*$)", std::regex::icase | std::regex::multiline);
    if (std::regex_search(script, m, optExp))
        return script.substr(0, m.position() + m.length()) + "\n" + STANDUP_TARGET_CLASS + script.substr(m.position() + m.length());
    return std::string(STANDUP_TARGET_CLASS) + script;
}

std::string ScriptPatcher::PatchDTArrayDefinitions(const std::string& script) {
    std::string r = script;
    std::regex p5(R"(\b(DT\d+)\s*=\s*Array\s*\(\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,\)]+)\s*\))", std::regex::icase);
    r = std::regex_replace(r, p5, "Set $1 = (new DropTarget)($2, $3, $4, $5, $6, false)");
    std::regex p6(R"(\b(DT\d+)\s*=\s*Array\s*\(\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,\)]+)\s*\))", std::regex::icase);
    r = std::regex_replace(r, p6, "Set $1 = (new DropTarget)($2, $3, $4, $5, $6, $7)");
    return r;
}

std::string ScriptPatcher::PatchSTArrayDefinitions(const std::string& script) {
    std::string r = script;
    std::regex p(R"(\b(ST\d+)\s*=\s*Array\s*\(\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,]+)\s*,\s*([^,\)]+)\s*\))", std::regex::icase);
    r = std::regex_replace(r, p, "Set $1 = (new StandupTarget)($2, $3, $4, $5, $6)");
    return r;
}

std::string ScriptPatcher::PatchDTArrayAccess(const std::string& script) {
    std::string r = script;
    std::regex d0(R"(DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*0\s*\))", std::regex::icase);
    r = std::regex_replace(r, d0, "DTArray($1).primary");
    std::regex d1(R"(DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*1\s*\))", std::regex::icase);
    r = std::regex_replace(r, d1, "DTArray($1).secondary");
    std::regex d2(R"(DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*2\s*\))", std::regex::icase);
    r = std::regex_replace(r, d2, "DTArray($1).prim");
    std::regex d3(R"(DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*3\s*\))", std::regex::icase);
    r = std::regex_replace(r, d3, "DTArray($1).sw");
    std::regex d4(R"(DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*4\s*\))", std::regex::icase);
    r = std::regex_replace(r, d4, "DTArray($1).animate");
    std::regex d5(R"(DTArray\s*\(\s*(\w+)\s*\)\s*\(\s*5\s*\))", std::regex::icase);
    r = std::regex_replace(r, d5, "DTArray($1).isDropped");
    return r;
}

std::string ScriptPatcher::PatchSTArrayAccess(const std::string& script) {
    std::string r = script;
    std::regex s0(R"(STArray\s*\(\s*(\w+)\s*\)\s*\(\s*0\s*\))", std::regex::icase);
    r = std::regex_replace(r, s0, "STArray($1).primary");
    std::regex s1(R"(STArray\s*\(\s*(\w+)\s*\)\s*\(\s*1\s*\))", std::regex::icase);
    r = std::regex_replace(r, s1, "STArray($1).prim");
    std::regex s2(R"(STArray\s*\(\s*(\w+)\s*\)\s*\(\s*2\s*\))", std::regex::icase);
    r = std::regex_replace(r, s2, "STArray($1).sw");
    std::regex s3(R"(STArray\s*\(\s*(\w+)\s*\)\s*\(\s*3\s*\))", std::regex::icase);
    r = std::regex_replace(r, s3, "STArray($1).animate");
    std::regex s4(R"(STArray\s*\(\s*(\w+)\s*\)\s*\(\s*4\s*\))", std::regex::icase);
    r = std::regex_replace(r, s4, "STArray($1).target");
    return r;
}

bool ScriptPatcher::UsesControllerPause(const std::string& script) {
    std::regex p(R"(Controller\.Pause\s*=)", std::regex::icase);
    return std::regex_search(script, p);
}

std::string ScriptPatcher::PatchControllerPause(const std::string& script) {
    std::string r = script;

    // First handle colon-separated statements (e.g., Sub Foo:Controller.Pause = True:End Sub)
    // These can't be safely commented out, so remove them entirely
    // Pattern matches :Controller.Pause = Value followed by :
    std::regex p1(R"(:[ 	]*Controller\.Pause\s*=\s*(True|False)[ 	]*:)", std::regex::icase);
    r = std::regex_replace(r, p1, ":");

    // Then handle statements on their own lines (comment them out)
    std::regex p2(R"((\s*)(Controller\.Pause\s*=\s*(True|False)))", std::regex::icase);
    r = std::regex_replace(r, p2, "$1' $2 ' Wine/Android");

    return r;
}

bool ScriptPatcher::UsesPuPlayerPlaystopInPlayclear(const std::string& script) {
    std::regex p(R"(if\s+chan\s*=\s*pBackglass\s+Then[\s\S]*?PuPlayer\.playstop\s+pDMD)", std::regex::icase);
    return std::regex_search(script, p);
}

std::string ScriptPatcher::PatchPuPlayerPlaystopInPlayclear(const std::string& script) {
    std::string r = script;
    std::regex p(R"((if\s+chan\s*=\s*pBackglass\s+Then\s*[\r\n]+)([ \t]*)(PuPlayer\.playstop\s+pDMD))", std::regex::icase);
    r = std::regex_replace(r, p, "$1$2' $3 ' Android");
    return r;
}
std::string ScriptPatcher::StripBOM(const std::string& script) {
    if (script.length() >= 3 && (unsigned char)script[0] == 0xEF && (unsigned char)script[1] == 0xBB && (unsigned char)script[2] == 0xBF) {
        PLOGI.printf("ScriptPatcher: Stripping UTF-8 BOM");
        return script.substr(3);
    }
    if (script.length() >= 2 && (unsigned char)script[0] == 0xFF && (unsigned char)script[1] == 0xFE) {
        PLOGI.printf("ScriptPatcher: Stripping UTF-16 LE BOM");
        return script.substr(2);
    }
    if (script.length() >= 2 && (unsigned char)script[0] == 0xFE && (unsigned char)script[1] == 0xFF) {
        PLOGI.printf("ScriptPatcher: Stripping UTF-16 BE BOM");
        return script.substr(2);
    }
    return script;
}

std::string ScriptPatcher::PatchAddScoreParentheses(const std::string& script) {
    std::string r = script;
    std::regex p(R"(AddScore\s+\(([^)]+\([^)]*\)[^)]*)\)\s*\+\s*(\w+\([^)]*\)))", std::regex::icase);
    r = std::regex_replace(r, p, "AddScore (($1)+$2)");
    return r;
}

std::string ScriptPatcher::PatchSetAlignedPositionParentheses(const std::string& script) {
    std::string r = script;
    std::regex p(R"(\.SetAlignedPosition\s+\(\(([^)]+)\)\*(\d+)\)\+(\d+)\s*,)", std::regex::icase);
    r = std::regex_replace(r, p, ".SetAlignedPosition ((($1)*$2)+$3),");
    return r;
}

std::string ScriptPatcher::PatchLineContinuationBeforeDot(const std::string& script) {
    std::string r = script;
    std::regex p(R"((\w|\))\s+_\s*\r?\n\s*\.)", std::regex::icase);
    r = std::regex_replace(r, p, "$1. _\n");
    return r;
}


// Wine VBScript Array Compatibility Helpers
// Wine's VBScript has issues with UBound on uninitialized arrays and 2D array access

bool ScriptPatcher::UsesProblematicArrays(const std::string& script) {
    // Check for patterns that cause Wine VBScript issues:
    // 1. UBound in If conditions before ReDim
    // 2. 2D array access
    std::regex p1(R"(If\s+UBound\s*\()", std::regex::icase);
    std::regex p2(R"(\w+\s*\(\s*\d+\s*,\s*\d+\s*\))", std::regex::icase);
    return std::regex_search(script, p1) || std::regex_search(script, p2);
}

std::string ScriptPatcher::InjectWineArrayHelpers(const std::string& script) {
    // Inject helper functions at the start of the script (after Option Explicit if present)
    // Check if Atn2 is USED in the script (called as a function)
    bool usesAtn2 = std::regex_search(script, std::regex(R"(\bAtn2\s*\()", std::regex::icase));
    // Check if Atn2 is properly DEFINED (after newline + optional whitespace, not in comment)
    // C++ regex doesn't support multiline ^, so we check for \n or start of string
    bool hasAtn2 = std::regex_search(script, std::regex(R"((^|\n)\s*Function\s+Atn2\b)", std::regex::icase));

    std::string helpers = R"(
' Wine VBScript Array Compatibility Helpers (Auto-injected by ScriptPatcher)
Function VPX_SafeUBound(arr)
    On Error Resume Next
    VPX_SafeUBound = -1
    VPX_SafeUBound = UBound(arr)
    On Error Goto 0
End Function

' Safe TypeName wrapper - avoids crash on Dictionary objects (emulated classes)
Function VPX_SafeTypeName(obj)
    On Error Resume Next
    Dim hasClass : hasClass = obj.Exists("__class__")
    If Err.Number = 0 Then
        If hasClass Then
            VPX_SafeTypeName = obj("__class__")
            On Error Goto 0
            Exit Function
        End If
        ' obj has Exists method but no __class__ - raw Dictionary
        VPX_SafeTypeName = "Dictionary"
        On Error Goto 0
        Exit Function
    End If
    Err.Clear
    On Error Goto 0
    ' Not a Dictionary - safe to call TypeName
    VPX_SafeTypeName = TypeName(obj)
End Function

Function VPX_SafeArrayGet(arr, idx)
    On Error Resume Next
    VPX_SafeArrayGet = Empty
    VPX_SafeArrayGet = arr(idx)
    On Error Goto 0
End Function

Function VPX_SafeArray2DGet(arr, idx1, idx2)
    On Error Resume Next
    VPX_SafeArray2DGet = Empty
    VPX_SafeArray2DGet = arr(idx1, idx2)
    On Error Goto 0
End Function

Sub VPX_SafeArraySet(arr, idx, val)
    On Error Resume Next
    arr(idx) = val
    On Error Goto 0
End Sub

Sub VPX_SafeReDim(ByRef arr, newSize)
    On Error Resume Next
    ReDim Preserve arr(newSize)
    On Error Goto 0
End Sub

Function VPX_ArrObj(arr, idx)
    On Error Resume Next
    Set VPX_ArrObj = arr(idx)
    On Error Goto 0
End Function

' Helper for chained Dictionary/array assignment: dict("key")(idx) = val
Sub VPX_SetDictArrItem(dict, key, idx, val)
    On Error Resume Next
    Dim arr : arr = dict(key)
    arr(idx) = val
    dict(key) = arr
    On Error Goto 0
End Sub

' Helper for chained Dictionary/array read: dict("key")(idx)
Function VPX_GetDictArrItem(dict, key, idx)
    On Error Resume Next
    VPX_GetDictArrItem = Empty
    Dim arr : arr = dict(key)
    VPX_GetDictArrItem = arr(idx)
    On Error Goto 0
End Function
)";

    // Only add Atn2 if script doesn't define its own
    std::string atn2Helper = R"(
Function Atn2(y, x)
    Dim PI: PI = 3.14159265358979
    If x > 0 Then
        Atn2 = Atn(y / x)
    ElseIf x < 0 Then
        If y >= 0 Then
            Atn2 = Atn(y / x) + PI
        Else
            Atn2 = Atn(y / x) - PI
        End If
    Else
        If y > 0 Then
            Atn2 = PI / 2
        ElseIf y < 0 Then
            Atn2 = -PI / 2
        Else
            Atn2 = 0
        End If
    End If
End Function
)";

    // Inject Atn2 if it's USED but not properly DEFINED
    if (usesAtn2 && !hasAtn2) {
        helpers += atn2Helper;
        PLOGI.printf("ScriptPatcher: Injecting Atn2 helper (used but not defined)");
    } else if (hasAtn2) {
        PLOGI.printf("ScriptPatcher: Script already defines Atn2, skipping injection");
    }

    helpers += "' End Wine VBScript Array Compatibility Helpers\n\n";

    std::string r = script;
    
    // Find insertion point - after Option Explicit or at start
    std::regex optionExplicit(R"((Option\s+Explicit[^\r\n]*[\r\n]+))", std::regex::icase);
    std::smatch match;
    if (std::regex_search(r, match, optionExplicit)) {
        r = match.prefix().str() + match[0].str() + helpers + match.suffix().str();
    } else {
        // Insert at the very beginning
        r = helpers + r;
    }
    
    return r;
}

// Inject VPX_SetArrObjProp and VPX_GetArrObjProp AFTER array property transformations
std::string ScriptPatcher::InjectVPXSetArrObjProp(const std::string& script) {
    // These helpers must be injected AFTER PatchArrayObjectPropertyAccess and PatchArrayObjectPropertyRead run,
    // otherwise the arr(idx).prop patterns inside these helpers would be transformed
    // into recursive calls, causing infinite recursion.
    std::string helper = R"(
' VPX_GetArrObjProp - Injected after array property read transformation
Function VPX_GetArrObjProp(arr, idx, propName)
    On Error Resume Next
    VPX_GetArrObjProp = Empty
    Select Case LCase(propName)
        Case "x": VPX_GetArrObjProp = arr(idx).x
        Case "y": VPX_GetArrObjProp = arr(idx).y
        Case "z": VPX_GetArrObjProp = arr(idx).z
        Case "velx": VPX_GetArrObjProp = arr(idx).velx
        Case "vely": VPX_GetArrObjProp = arr(idx).vely
        Case "velz": VPX_GetArrObjProp = arr(idx).velz
        Case "id": VPX_GetArrObjProp = arr(idx).id
        Case "visible": VPX_GetArrObjProp = arr(idx).visible
        Case "state": VPX_GetArrObjProp = arr(idx).state
        Case "height": VPX_GetArrObjProp = arr(idx).height
        Case "mass": VPX_GetArrObjProp = arr(idx).mass
        Case "radius": VPX_GetArrObjProp = arr(idx).radius
        Case "name": VPX_GetArrObjProp = arr(idx).name
        Case "image": VPX_GetArrObjProp = arr(idx).image
        Case "bulbhaloheight": VPX_GetArrObjProp = arr(idx).BulbHaloHeight
        Case "intensity": VPX_GetArrObjProp = arr(idx).intensity
    End Select
    On Error Goto 0
End Function

' VPX_SetArrObjProp - Injected after array property access transformation
Sub VPX_SetArrObjProp(arr, idx, propName, val)
    On Error Resume Next
    Select Case LCase(propName)
        Case "visible": arr(idx).visible = val
        Case "x": arr(idx).x = val
        Case "y": arr(idx).y = val
        Case "z": arr(idx).z = val
        Case "height": arr(idx).height = val
        Case "opacity": arr(idx).opacity = val
        Case "state": arr(idx).state = val
        Case "bulbhaloheight": arr(idx).BulbHaloHeight = val
        Case "intensity": arr(idx).intensity = val
        Case "image": arr(idx).image = val
        Case "collidable": arr(idx).collidable = val
        Case "rotation": arr(idx).rotation = val
        Case "rotz": arr(idx).rotz = val
        Case "rotx": arr(idx).rotx = val
        Case "roty": arr(idx).roty = val
        Case "size": arr(idx).size = val
        Case "enabled": arr(idx).enabled = val
        Case "timerinterval": arr(idx).timerinterval = val
        Case "timerenabled": arr(idx).timerenabled = val
        Case "uservalue": arr(idx).uservalue = val
        Case "color": arr(idx).color = val
        Case "falloff": arr(idx).falloff = val
        Case "falloffpower": arr(idx).falloffpower = val
        Case "blend": arr(idx).blend = val
        Case "material": arr(idx).material = val
        Case "isdropped": arr(idx).isdropped = val
        Case "objrotz": arr(idx).objrotz = val
        Case "transx": arr(idx).transx = val
        Case "transy": arr(idx).transy = val
        Case "transz": arr(idx).transz = val
        Case "scalex": arr(idx).scalex = val
        Case "scaley": arr(idx).scaley = val
        Case "scalez": arr(idx).scalez = val
    End Select
    On Error Goto 0
End Sub
)";

    std::string r = script;

    // Find insertion point - after Option Explicit or at start
    std::regex optionExplicit(R"((Option\s+Explicit[^\r\n]*[\r\n]+))", std::regex::icase);
    std::smatch match;
    if (std::regex_search(r, match, optionExplicit)) {
        r = match.prefix().str() + match[0].str() + helper + match.suffix().str();
    } else {
        // Insert at the very beginning
        r = helper + r;
    }

    return r;
}

std::string ScriptPatcher::PatchUBoundInConditions(const std::string& script) {
    std::string r = script;

    // Replace "If UBound(array)" with "If VPX_SafeUBound(array)"
    // Pattern: If UBound(varname) followed by comparison
    std::regex p(R"(If\s+UBound\s*\(\s*(\w+)\s*\))", std::regex::icase);
    r = std::regex_replace(r, p, "If VPX_SafeUBound($1)");

    return r;
}

std::string ScriptPatcher::PatchUBoundInForLoops(const std::string& script) {
    std::string r = script;

    // Replace "For x = ... to UBound(array)" with "For x = ... to VPX_SafeUBound(array)"
    // This handles uninitialized arrays that would throw on UBound()
    std::regex p(R"(\bto\s+UBound\s*\(\s*(\w+)\s*\))", std::regex::icase);
    r = std::regex_replace(r, p, "to VPX_SafeUBound($1)");

    return r;
}

std::string ScriptPatcher::PatchReDimWithUBound(const std::string& script) {
    std::string r = script;
    
    // Pattern: If UBound(arr) < val Then ReDim arr(val)
    // This pattern is problematic because UBound fails on uninitialized arrays
    // We've already patched UBound -> VPX_SafeUBound, so the If check should work
    // But we also need to ensure the arrays are initialized
    
    // For now, the SafeUBound should handle most cases
    return r;
}

std::string ScriptPatcher::Patch2DArrayAccess(const std::string& script) {
    std::string r = script;
    
    // Pattern: if not ArrayName(0,0) then ... 
    // This is problematic in Wine VBScript - wrap in error handling by commenting out
    // or replacing with a safe version
    
    // For "if not ArrayName(num,num)" patterns, we need to be careful
    // Let's wrap specific known problematic patterns
    
    // RampBalls(0,0) pattern - this is checking if array element is falsy
    // Replace with a try-catch style approach or just default to a safe value
    std::regex p(R"(if\s+not\s+(\w+)\s*\(\s*(\d+)\s*,\s*(\d+)\s*\)\s+then)", std::regex::icase);
    r = std::regex_replace(r, p, "If Not VPX_SafeArray2DGet($1, $2, $3) Then");
    
    return r;
}

std::string ScriptPatcher::PatchArrayElementAssignment(const std::string& script) {
    std::string r = script;

    // Pattern: arrayName(index) = value where array might not be initialized
    // Wine VBScript has issues with array element assignments in certain contexts
    //
    // IMPORTANT: Only match ASSIGNMENTS at statement start, not comparisons in If conditions!
    // Statement start positions: line start, after :, after Then, after Else
    //
    // Example that must NOT be transformed (comparison in If condition):
    //   If rolling(b) = True Then ...
    // Example that SHOULD be transformed (assignment):
    //   rolling(b) = True
    //
    // Use inline error handling to avoid needing helper function at runtime:
    //   rolling(b) = False -> On Error Resume Next : rolling(b) = False : On Error Goto 0

    // For known problematic arrays with True/False values - only at statement start
    // Use inline error handling instead of VPX_SafeArraySet
    std::regex p1(R"((^[ \t]*)(bBallInTrough|rolling)\s*\(\s*(\w+)\s*\)\s*=\s*(True|False))",
                  std::regex::icase | std::regex::multiline);
    r = std::regex_replace(r, p1, "$1On Error Resume Next : $2($3) = $4 : On Error Goto 0");

    // After Then/Else - use colon-separated inline
    std::regex p2(R"((\bThen[ \t]+|\bElse[ \t]+)(bBallInTrough|rolling)\s*\(\s*(\w+)\s*\)\s*=\s*(True|False))",
                  std::regex::icase);
    r = std::regex_replace(r, p2, "$1On Error Resume Next : $2($3) = $4 : On Error Goto 0");

    // NOTE: ballvel/ballvelx/ballvely are now handled by class emulation
    // (they become CoRTracker_ballvel etc. as global arrays)
    return r;
}

std::string ScriptPatcher::PatchDictArrayAccess(const std::string& script) {
    std::string r = script;

    // Wine VBScript doesn't support chained Dictionary/array access: dict("key")(idx)
    // This pattern appears in emulated classes where properties are arrays
    // Transform: varName("key")(idx) = value
    // To: VPX_SetDictArrItem varName, "key", idx, value

    // Pattern for assignment: dict("key")(idx) = value (at statement start)
    // Note: C++ regex ^ only matches start of STRING, not line. Use (^|\n) instead.
    // Use custom raw string delimiter to avoid issues with quotes inside
    std::regex assignPattern(R"REGEX((^|\n)([ \t]*)(\w+)\s*\(\s*"([^"]+)"\s*\)\s*\(\s*([^)]+)\s*\)\s*=\s*([^\r\n:]+))REGEX",
                             std::regex::icase);
    r = std::regex_replace(r, assignPattern, "$1$2VPX_SetDictArrItem $3, \"$4\", $5, $6");

    // Also handle after colon (statement separator)
    std::regex colonPattern(R"REGEX((:[ \t]*)(\w+)\s*\(\s*"([^"]+)"\s*\)\s*\(\s*([^)]+)\s*\)\s*=\s*([^\r\n:]+))REGEX",
                             std::regex::icase);
    r = std::regex_replace(r, colonPattern, "$1VPX_SetDictArrItem $2, \"$3\", $4, $5");

    PLOGI.printf("ScriptPatcher: Applied Dictionary/array access patch");
    return r;
}

std::string ScriptPatcher::PatchArrayObjectPropertyAccess(const std::string& script) {
    std::string r = script;

    // Wine VBScript doesn't support Array(idx).property = value syntax
    // Transform: ArrayName(idx).property = value
    // To: VPX_SetArrObjProp ArrayName, idx, "property", value
    //
    // VPX_SetArrObjProp is a helper sub that sets the property with error handling.
    // It uses a Select Case for common properties and Execute for unknown ones.
    //
    // IMPORTANT: Only match ASSIGNMENTS at statement start, not comparisons in If conditions!
    // Statement start positions: line start, after :, after Then, after Else
    //
    // Example that must NOT be transformed (comparison in If condition):
    //   If Glowing(b).state = 0 Then ...
    // Example that SHOULD be transformed (assignment after Then):
    //   If x Then Glowing(b).state = 1

    // Pattern requires statement start position to avoid matching comparisons
    // Value capture stops at : or Then/Else keywords or newline (non-greedy)
    std::regex p(R"((^[ \t]*|:[ \t]*|\bThen[ \t]+|\bElse[ \t]+)(\w+)\s*\(\s*([^)]+)\s*\)\s*\.(\w+)\s*=\s*([^:\r\n]+?)(?=[ \t]*(?::|'|\bThen\b|\bElse\b|\r|\n|$)))",
                 std::regex::icase | std::regex::multiline);
    r = std::regex_replace(r, p, "$1VPX_SetArrObjProp $2, $3, \"$4\", $5");

    return r;
}

std::string ScriptPatcher::PatchArrayObjectPropertyRead(const std::string& script) {
    std::string r = script;

    // Wine VBScript doesn't support Array(idx).property READ syntax either
    // Transform: ArrayName(idx).property
    // To: VPX_GetArrObjProp(ArrayName, idx, "property")
    //
    // IMPORTANT: Only transform if property is a KNOWN VPX object property.
    // This avoids breaking object method chains like Games(x).Settings

    // List of known VPX object properties that should be transformed
    static const std::unordered_set<std::string> vpxProperties = {
        "x", "y", "z", "velx", "vely", "velz", "id", "visible", "state",
        "height", "mass", "radius", "name", "image", "bulbhaloheight",
        "intensity", "opacity", "collidable", "rotation", "rotz", "rotx",
        "roty", "size", "enabled", "timerinterval", "timerenabled",
        "uservalue", "color", "falloff", "falloffpower", "blend",
        "material", "isdropped", "objrotz", "objrotx", "objroty",
        "transx", "transy", "transz", "scalex", "scaley", "scalez",
        "intensityscale"
    };

    // List of VBScript built-in functions to exclude
    static const std::unordered_set<std::string> excludedFunctions = {
        "abs", "atn", "cos", "sin", "tan", "exp", "log", "sqr", "int", "fix",
        "sgn", "rnd", "round", "hex", "oct", "chr", "asc", "len", "mid", "left",
        "right", "instr", "instrrev", "lcase", "ucase", "ltrim", "rtrim", "trim",
        "space", "string", "replace", "split", "join", "cint", "clng", "csng",
        "cdbl", "cstr", "cbool", "cdate", "dateadd", "datediff", "datepart",
        "dateserial", "datevalue", "day", "month", "year", "hour", "minute",
        "second", "now", "date", "time", "timer", "weekday", "monthname",
        "isarray", "isdate", "isempty", "isnull", "isnumeric", "isobject",
        "typename", "vartype", "array", "filter", "getref", "eval", "execute",
        "executeglobal", "msgbox", "inputbox", "createobject", "getobject",
        "rgb", "ubound", "lbound", "redim", "erase"
    };

    // Pattern: word(index).property - but we'll check exclusions in callback
    std::regex p(R"((\w+)\s*\(\s*([^)]+)\s*\)\s*\.(\w+)\b)", std::regex::icase);

    std::string result;
    std::sregex_iterator it(r.begin(), r.end(), p);
    std::sregex_iterator end;
    size_t lastPos = 0;

    while (it != end) {
        std::smatch match = *it;
        result += r.substr(lastPos, match.position() - lastPos);

        std::string funcName = match[1].str();
        std::string funcNameLower = funcName;
        std::transform(funcNameLower.begin(), funcNameLower.end(), funcNameLower.begin(), ::tolower);

        std::string indexExpr = match[2].str();
        std::string propName = match[3].str();
        std::string propNameLower = propName;
        std::transform(propNameLower.begin(), propNameLower.end(), propNameLower.begin(), ::tolower);

        // Only transform if:
        // 1. Function name is NOT a VBScript built-in
        // 2. Function name is NOT already a VPX_ function
        // 3. Property name IS a known VPX property
        // 4. Index expression does NOT contain '(' (nested function calls)
        if (excludedFunctions.count(funcNameLower) > 0 ||
            funcNameLower.substr(0, 4) == "vpx_" ||
            vpxProperties.count(propNameLower) == 0 ||
            indexExpr.find('(') != std::string::npos) {
            // Keep original
            result += match[0].str();
        } else {
            // Transform to VPX_GetArrObjProp
            // Wrap in extra parens to force expression evaluation (fixes Wine VBScript parser issue
            // when this appears as first argument in Sub call like: UpdateMaterial VPX_GetArrObjProp(...), ...)
            result += "(VPX_GetArrObjProp(" + match[1].str() + ", " + match[2].str() + ", \"" + match[3].str() + "\"))";
        }

        lastPos = match.position() + match.length();
        ++it;
    }
    result += r.substr(lastPos);

    return result;
}

std::string ScriptPatcher::PatchSingleLineIfElse(const std::string& script) {
    std::string r = script;
    // Use [ \t]+ instead of \s+ to avoid matching across newlines
    std::regex p(R"((Then[ \t]+[^'\r\n]+[ \t]+Else)[ \t]*('.*)?$)", std::regex::icase | std::regex::multiline);
    r = std::regex_replace(r, p, "$1:$2");
    return r;
}

std::string ScriptPatcher::PatchExecuteEval(const std::string& script) { return script; }

std::string ScriptPatcher::PatchStringConcatenation(const std::string& script) {
    std::string r = script;
    std::regex p(R"(\(\s*(\([^)]+\))\s*&\s*")", std::regex::icase);
    r = std::regex_replace(r, p, R"(("" & $1 & ")");
    return r;
}

bool ScriptPatcher::UsesSlingshotCorrection(const std::string& script) {
    std::regex p(R"(Class\s+(SlingshotCorrection|FlipperPolarity|FlipperPhysics|BumperPhysics))", std::regex::icase);
    return std::regex_search(script, p);
}

std::string ScriptPatcher::PatchSlingshotCorrection(const std::string& script) {
    // Legacy - now handled by EmulateClasses
    return script;
}

// ============================================================================
// MAIN ENTRY POINT
// ============================================================================

std::string ScriptPatcher::PatchScript(const std::string& script) {
    std::string result = StripBOM(script);
    bool patched = result.length() != script.length();
    PLOGI.printf("ScriptPatcher: Checking script (length=%zu)", result.length());

    // Class emulation (must run first)
    if (HasClassDefinitions(result)) {
        std::string before = result;
        result = EmulateClasses(result);
        if (result != before) {
            PLOGI.printf("ScriptPatcher: Applied class emulation");
            patched = true;

            // Replace TypeName with VPX_SafeTypeName to avoid crashes on Dictionary objects
            std::regex typeNameRegex(R"(\bTypeName\s*\()", std::regex::icase);
            result = std::regex_replace(result, typeNameRegex, "VPX_SafeTypeName(");
            PLOGI.printf("ScriptPatcher: Replaced TypeName with VPX_SafeTypeName");
        }
    }

    // DTArray/STArray
    if (UsesDTArray(result)) {
        PLOGI.printf("ScriptPatcher: Applying DTArray patches");
        result = InjectDropTargetClass(result);
        result = PatchDTArrayDefinitions(result);
        result = PatchDTArrayAccess(result);
        patched = true;
    }
    if (UsesSTArray(result)) {
        PLOGI.printf("ScriptPatcher: Applying STArray patches");
        result = InjectStandupTargetClass(result);
        result = PatchSTArrayDefinitions(result);
        result = PatchSTArrayAccess(result);
        patched = true;
    }

    // Other patches
    if (UsesControllerPause(result)) { result = PatchControllerPause(result); patched = true; }
    if (UsesPuPlayerPlaystopInPlayclear(result)) { result = PatchPuPlayerPlaystopInPlayclear(result); patched = true; }

    { std::string b = result; result = PatchAddScoreParentheses(result); if (result != b) patched = true; }
    { std::string b = result; result = PatchSetAlignedPositionParentheses(result); if (result != b) patched = true; }
    // Disabled for testing
    // { std::string b = result; result = PatchLineContinuationBeforeDot(result); if (result != b) patched = true; }
    // { std::string b = result; result = PatchSingleLineIfElse(result); if (result != b) patched = true; }
    // Disabled: PatchStringConcatenation has a broken regex that corrupts scripts
    // { std::string b = result; result = PatchStringConcatenation(result); if (result != b) patched = true; }

    // Wine VBScript Array Compatibility patches
    if (UsesProblematicArrays(result)) {
        PLOGI.printf("ScriptPatcher: Applying Wine array compatibility patches");
        result = InjectWineArrayHelpers(result);
        result = PatchUBoundInConditions(result);
        result = PatchUBoundInForLoops(result);
        result = PatchReDimWithUBound(result);
        result = Patch2DArrayAccess(result);
        result = PatchArrayElementAssignment(result);
        result = PatchDictArrayAccess(result);
        result = PatchArrayObjectPropertyAccess(result);
        // DISABLED: PatchArrayObjectPropertyRead causes compile errors when VPX_GetArrObjProp is used as
        // argument in Sub calls. Wine VBScript's parser doesn't handle this well.
        // For native VPX objects, arr(idx).property works fine. Only emulated classes have issues.
        // result = PatchArrayObjectPropertyRead(result);
        // IMPORTANT: Inject VPX_SetArrObjProp and VPX_GetArrObjProp AFTER the transformations
        // to avoid the helpers' arr(idx).prop patterns being transformed into recursive calls
        result = InjectVPXSetArrObjProp(result);
        patched = true;
    }

    if (patched) {
        PLOGI.printf("ScriptPatcher: Complete (length=%zu)", result.length());
        // Debug: dump patched script to file for inspection
        FILE* debugFile = fopen("/storage/emulated/0/Download/patched_script.vbs", "w");
        if (debugFile) {
            fwrite(result.c_str(), 1, result.length(), debugFile);
            fclose(debugFile);
            PLOGI.printf("ScriptPatcher: Dumped patched script to /storage/emulated/0/Download/patched_script.vbs");
        }
    }
    return result;
}

#endif // __STANDALONE__
