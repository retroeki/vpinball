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
            std::istringstream varStream(varList);
            std::string varToken;
            while (std::getline(varStream, varToken, ',')) {
                varToken = Trim(varToken);
                bool isArray = false;
                size_t parenPos = varToken.find('(');
                if (parenPos != std::string::npos) {
                    isArray = true;  // Property declared as array (e.g., "Private arr()")
                    varToken = varToken.substr(0, parenPos);
                }
                varToken = Trim(varToken);
                if (!varToken.empty()) {
                    VBClassProperty prop;
                    prop.name = varToken;
                    prop.isPublic = isPublic;
                    prop.isArray = isArray;
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
            std::string propPattern = "\\b" + escapedName + "\\b";
            std::regex allPattern(propPattern, std::regex::icase);
            result = std::regex_replace(result, allPattern, globalName);
        } else {
            // Non-array properties: use Dictionary-based approach for assignments
            std::string propPattern = "\\b" + escapedName + "\\s*=\\s*";
            std::regex assignPattern(propPattern, std::regex::icase);
            result = std::regex_replace(result, assignPattern, "this_(\"" + prop.name + "\") = ");
        }
    }
    return result;
}

std::string ScriptPatcher::EmitClassEmulation(const VBClassDefinition& classDef) {
    std::ostringstream out;
    out << "' === " << classDef.name << " Class Emulation ===\n\n";

    // Emit global Dim statements for array properties (arrays can't be stored in Dictionary)
    for (const auto& prop : classDef.properties) {
        if (prop.isArray) {
            out << "Dim " << classDef.name << "_" << prop.name << "()\n";
        }
    }
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
        
        out << (method.isFunction ? "Function " : "Sub ") << classDef.name << "_" << method.name 
            << "(" << paramList << ")\n";
        std::string transformedBody = TransformMethodBody(method.body, classDef);
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
            out << "Function " << classDef.name << "_Get_" << accessor.name << "(" << paramList << ")\n";
            std::string tb = TransformMethodBody(accessor.body, classDef);
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
        std::string pattern = "(Set\\s+\\w+\\s*=\\s*)New\\s+" + escapedClassName + "\\b";
        std::regex newPattern(pattern, std::regex::icase);
        result = std::regex_replace(result, newPattern, "$1" + className + "_Create()");
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

            // Write: var.accessor(idx) = value  →  ClassName_Let_accessor var, idx, value
            // ONLY match at statement start to avoid matching comparisons in If statements
            // Statement start: line start (with optional indent), after :, after Then, after Else
            // Value capture must stop at : or Else (for single-line If statements)
            std::string wp = "(^[ \\t]*|:[ \\t]*|\\bThen\\s+|\\bElse\\s+)" + escapedVar + "\\." + escapedAcc + "\\s*\\(([^)]*)\\)\\s*=\\s*([^:\\r\\n]*?)(?=\\s*(?:Else\\b|:|\\r|\\n|$))";
            std::regex wr(wp, std::regex::icase | std::regex::multiline);
            result = std::regex_replace(result, wr, "$1" + className + "_Let_" + accName + " " + varName + ", $2, $3");

            // Read: var.accessor(idx)  →  ClassName_Get_accessor(var, idx)
            // Match all remaining accessor calls (those not converted to Let above are reads)
            std::string rp = escapedVar + "\\." + escapedAcc + "\\s*\\(([^)]*)\\)";
            std::regex rr(rp, std::regex::icase);
            result = std::regex_replace(result, rr, className + "_Get_" + accName + "(" + varName + ", $1)");
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
    for (auto& cls : classes) {
        // Collect all method/accessor bodies plus initializeBody
        std::string allBodies = cls.initializeBody + "\n";
        for (const auto& m : cls.methods) allBodies += m.body + "\n";
        for (const auto& a : cls.accessors) allBodies += a.body + "\n";

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

    std::string result = script;
    for (const auto& cls : classes) {
        std::string emulation = EmitClassEmulation(cls);
        result = result.substr(0, cls.startPos) + emulation + result.substr(cls.endPos);
    }

    result = TransformNewStatements(result, classNames);
    result = TransformMethodCalls(result, classes);
    result = TransformPropertyAccess(result, classes);
    result = TransformAccessorAccess(result, classes);
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
    std::string helpers = R"(
' Wine VBScript Array Compatibility Helpers (Auto-injected by ScriptPatcher)
Function VPX_SafeUBound(arr)
    On Error Resume Next
    VPX_SafeUBound = -1
    VPX_SafeUBound = UBound(arr)
    On Error Goto 0
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
' End Wine VBScript Array Compatibility Helpers

)";

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
    // IMPORTANT: Must NOT transform:
    // - VBScript built-in functions: Atn(), Abs(), Sin(), Cos(), etc.
    // - Already transformed VPX_ functions
    // - Object method chains like Games(x).Settings

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

        // Check if this is an excluded function or already a VPX_ function
        if (excludedFunctions.count(funcNameLower) > 0 ||
            funcNameLower.substr(0, 4) == "vpx_") {
            // Keep original
            result += match[0].str();
        } else {
            // Transform to VPX_GetArrObjProp
            result += "VPX_GetArrObjProp(" + match[1].str() + ", " + match[2].str() + ", \"" + match[3].str() + "\")";
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
        if (result != before) { PLOGI.printf("ScriptPatcher: Applied class emulation"); patched = true; }
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
        result = PatchArrayObjectPropertyAccess(result);
        result = PatchArrayObjectPropertyRead(result);
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
