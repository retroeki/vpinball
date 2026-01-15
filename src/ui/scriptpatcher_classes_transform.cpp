/**
 * @file scriptpatcher_classes_transform.cpp
 * @brief VBScript Class Usage Transformation
 *
 * Transforms VBScript code that uses emulated classes:
 * - TransformNewStatements: Set x = New ClassName -> Set x = ClassName_Create()
 * - TransformMethodCalls: obj.Method(args) -> ClassName_Method(obj, args)
 * - TransformPropertyAccess: obj.Prop -> obj("Prop")
 * - TransformAccessorAccess: obj.accessor(idx) -> ClassName_Get_accessor(obj, idx)
 * - EmulateClasses: Main orchestrator that coordinates all transformations
 *
 * Uses Google RE2 for regex operations (much faster than std::regex)
 */

#include "stdafx.h"

#ifdef __STANDALONE__

#include "scriptpatcher.h"
#include "scriptpatcher_internal.h"
#include <sstream>

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
        // If args is empty, omit the comma: ClassName_defaultMethod(ClassName_Create())
        auto it = defaultMethods.find(className);
        if (it != defaultMethods.end()) {
            std::string defaultMethodName = it->second;
            // Pattern: (new ClassName)(args) - with parentheses around "new ClassName"
            RE2 newPattern3("(?i)\\(\\s*new\\s+" + escapedClassName + "\\s*\\)\\s*\\(([^)]*)\\)");
            result = RE2ReplaceWithCallback(result, newPattern3, [&](const RE2Match& m) -> std::string {
                std::string args = m.groups.size() > 0 ? m.groups[0] : "";
                // Trim whitespace from args
                size_t start = args.find_first_not_of(" \t\r\n");
                size_t end = args.find_last_not_of(" \t\r\n");
                if (start == std::string::npos) {
                    args.clear();  // All whitespace
                } else {
                    args = args.substr(start, end - start + 1);
                }
                // If args is empty, don't add comma
                if (args.empty()) {
                    return className + "_" + defaultMethodName + "(" + className + "_Create())";
                } else {
                    return className + "_" + defaultMethodName + "(" + className + "_Create(), " + args + ")";
                }
            });
        }

        // Pattern 1: Simple variable assignment: Set varName = New ClassName
        RE2 newPattern1("(?i)(Set\\s+\\w+\\s*=\\s*)New\\s+" + escapedClassName + "\\b");
        result = RE2Replace(result, newPattern1, "\\1" + className + "_Create()");

        // Pattern 2: Array element assignment: Set arrayName(idx) = New ClassName
        RE2 newPattern2("(?i)(Set\\s+\\w+\\s*\\([^)]+\\)\\s*=\\s*)New\\s+" + escapedClassName + "\\b");
        result = RE2Replace(result, newPattern2, "\\1" + className + "_Create()");
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
 * OPTIMIZATION: This uses a single-pass approach with RE2FindAll instead of
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
        // Merge methods rather than overwriting - this handles stub classes correctly
        auto& methods = classMethods[cls.name];
        for (const auto& m : cls.methods) {
            std::string lowerName = ToLower(m.name);
            if (methods.find(lowerName) == methods.end()) {
                methods[lowerName] = m.name;  // Only add if not already present
                PLOGI.printf("ScriptPatcher: Class '%s' has method '%s'", cls.name.c_str(), m.name.c_str());
            }
        }
    }

    // =========================================================================
    // PHASE 2: Find all variables that hold emulated class instances
    // =========================================================================

    // Scan for: Set varName = ClassName_Create() or ClassName_init(ClassName_Create(), ...)
    // Build maps: lowercase var -> className, lowercase var -> original case var
    std::unordered_map<std::string, std::string> varTypes;       // lowerVar -> className
    std::unordered_map<std::string, std::string> varOriginalCase; // lowerVar -> originalVar
    for (const auto& cls : classes) {
        std::string escapedClassName = EscapeRegex(cls.name);

        // Pattern 1: Set varName = ClassName_Create()
        RE2 setPattern1("(?i)Set\\s+(\\w+)\\s*=\\s*" + escapedClassName + "_Create\\s*\\(");
        auto matches = RE2FindAll(script, setPattern1);
        for (const auto& match : matches) {
            std::string varName = match.groups.size() > 0 ? match.groups[0] : "";
            std::string lowerVar = ToLower(varName);
            varTypes[lowerVar] = cls.name;
            varOriginalCase[lowerVar] = varName;
        }

        // Pattern 2: Set varName = ClassName_defaultMethod(ClassName_Create(), ...)
        // For classes with default constructors
        RE2 setPattern2("(?i)Set\\s+(\\w+)\\s*=\\s*" + escapedClassName + "_\\w+\\s*\\(\\s*" + escapedClassName + "_Create\\s*\\(");
        matches = RE2FindAll(script, setPattern2);
        for (const auto& match : matches) {
            std::string varName = match.groups.size() > 0 ? match.groups[0] : "";
            std::string lowerVar = ToLower(varName);
            if (varTypes.find(lowerVar) == varTypes.end()) {
                varTypes[lowerVar] = cls.name;
                varOriginalCase[lowerVar] = varName;
            }
        }
    }

    PLOGI.printf("ScriptPatcher: TransformMethodCalls found %zu typed variables", varTypes.size());
    for (const auto& [var, cls] : varTypes) {
        PLOGI.printf("ScriptPatcher:   varTypes[%s] = %s", var.c_str(), cls.c_str());
    }

    // Early exit if no emulated class instances found
    if (varTypes.empty()) return script;

    // =========================================================================
    // PHASE 3: Single-pass transformation using RE2FindAll
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
    static const RE2 dotAccess(R"((?i)\b(\w+)\.(\w+)(\s*\(([^)]*)\)|[ \t]+([^'=:\r\n\s][^:\r\n]*))?)");

    std::string result;
    result.reserve(script.size() + script.size() / 10);  // Pre-allocate with 10% buffer

    auto matches = RE2FindAll(script, dotAccess);
    size_t lastPos = 0;

    for (const auto& match : matches) {
        std::string lowerVar = ToLower(match.groups.size() > 0 ? match.groups[0] : "");
        std::string lowerMember = ToLower(match.groups.size() > 1 ? match.groups[1] : "");

        // Append any text between last match and this match (unchanged)
        result.append(script, lastPos, match.position - lastPos);

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

                if (match.groups.size() > 2 && !match.groups[2].empty()) {
                    // IMPORTANT: Check if group 3 (parens args) or group 4 (space args) matched
                    if (match.groups.size() > 3 && !match.groups[3].empty()) {
                        // Parenthesized args: var.Method(args) or var.Method()
                        hasParens = true;
                        args = match.groups[3];
                    } else if (match.groups.size() > 4 && !match.groups[4].empty()) {
                        // Space-separated args: var.Method arg1, arg2
                        args = match.groups[4];
                        // Trim trailing whitespace (but preserve args content)
                        size_t endPos = args.find_last_not_of(" \t\r\n");
                        if (endPos != std::string::npos)
                            args = args.substr(0, endPos + 1);
                        else
                            args.clear();
                    }
                }
                // If match[2] not matched: no-args call like var.Method

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

                lastPos = match.position + match.length;
                continue;
            }
        }

        // Not a method call on emulated class - keep original text
        result += match.full_match;
        lastPos = match.position + match.length;
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
        RE2 setPattern("(?i)Set\\s+(\\w+)\\s*=\\s*" + escapedClassName + "_Create\\(\\)");
        auto matches = RE2FindAll(script, setPattern);
        for (const auto& match : matches) {
            if (match.groups.size() > 0) {
                varTypes[match.groups[0]] = cls.name;
            }
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
            RE2 ar("(?i)\\b" + escapedVar + "\\." + escapedProp + "\\s*\\(");
            result = RE2Replace(result, ar, className + "_" + propName + "(");
        }

        // Handle REGULAR properties: var.Prop -> var("Prop")
        const auto& props = classProps[className];
        for (const auto& propName : props) {
            std::string escapedProp = EscapeRegex(propName);
            // Read: var.Prop (not = )
            // Note: RE2 doesn't support negative lookahead (?!), so we handle it differently
            // We'll use a two-step approach: first mark assignments, then transform reads

            // Write: var.Prop =
            RE2 wr("(?i)\\b" + escapedVar + "\\." + escapedProp + "\\s*=");
            result = RE2Replace(result, wr, varName + "(\"" + propName + "\") =");

            // Set: Set var.Prop =
            RE2 sr("(?i)Set\\s+\\b" + escapedVar + "\\." + escapedProp + "\\s*=");
            result = RE2Replace(result, sr, "Set " + varName + "(\"" + propName + "\") =");

            // Read: var.Prop (remaining after write transforms)
            RE2 rr("(?i)\\b" + escapedVar + "\\." + escapedProp + "\\b");
            // Use callback to skip if followed by =
            result = RE2ReplaceWithCallback(result, rr, [&](const RE2Match& m) -> std::string {
                // Check if followed by = (already handled)
                return varName + "(\"" + propName + "\")";
            });
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
        RE2 setPattern("(?i)Set\\s+(\\w+)\\s*=\\s*" + escapedClassName + "_Create\\(\\)");
        auto matches = RE2FindAll(script, setPattern);
        for (const auto& match : matches) {
            if (match.groups.size() > 0) {
                varTypes[match.groups[0]] = cls.name;
            }
        }
    }

    std::string result = script;
    for (const auto& [varName, className] : varTypes) {
        const auto& accessors = classAccessors[className];
        std::string escapedVar = EscapeRegex(varName);

        for (const auto& [accName, accType] : accessors) {
            std::string escapedAcc = EscapeRegex(accName);

            // Write WITH params: var.accessor(idx) = value  ->  ClassName_Let_accessor var, idx, value
            // ONLY match at statement start to avoid matching comparisons in If statements
            // RE2 doesn't support lookahead, capture trailing and restore
            RE2 wr("(?im)(^[ \\t]*|:[ \\t]*|\\bThen\\s+|\\bElse\\s+)\\b" + escapedVar + "\\." + escapedAcc + "\\s*\\(([^()]*(?:\\([^()]*\\)[^()]*)*)\\)\\s*=\\s*([^:\\r\\n]*?)(\\s*(?:Else\\b|:|\\r|\\n|$))");
            result = RE2Replace(result, wr, "\\1" + className + "_Let_" + accName + " " + varName + ", \\2, \\3\\4");

            // Write WITHOUT params: var.accessor = value  ->  ClassName_Let_accessor var, value
            RE2 swr("(?im)(^[ \\t]*|:[ \\t]*|\\bThen\\s+|\\bElse\\s+)\\b" + escapedVar + "\\." + escapedAcc + "\\s*=\\s*([^:\\r\\n]*?)(\\s*(?:Else\\b|:|\\r|\\n|$))");
            result = RE2Replace(result, swr, "\\1" + className + "_Let_" + accName + " " + varName + ", \\2\\3");

            // Read WITH params: var.accessor(idx)  ->  ClassName_Get_accessor(var, idx)
            RE2 rr("(?i)\\b" + escapedVar + "\\." + escapedAcc + "\\s*\\(([^()]*(?:\\([^()]*\\)[^()]*)*)\\)");
            result = RE2Replace(result, rr, className + "_Get_" + accName + "(" + varName + ", \\1)");

            // Read WITHOUT params: var.accessor  ->  ClassName_Get_accessor(var)
            // Must not be followed by ( or = - check manually since RE2 doesn't support lookahead
            RE2 srr("(?i)\\b" + escapedVar + "\\." + escapedAcc + "\\b");
            result = RE2ReplaceWithCallback(result, srr, [&](const RE2Match& m) -> std::string {
                // Check what follows in the original script
                size_t afterPos = m.position + m.length;
                if (afterPos < result.length()) {
                    // Skip whitespace
                    while (afterPos < result.length() && (result[afterPos] == ' ' || result[afterPos] == '\t')) {
                        afterPos++;
                    }
                    if (afterPos < result.length() && (result[afterPos] == '(' || result[afterPos] == '=')) {
                        return m.full_match;  // Keep original
                    }
                }
                return className + "_Get_" + accName + "(" + varName + ")";
            });
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
                RE2 redimPattern("(?i)\\bReDim\\s+" + EscapeRegex(prop.name) + "\\s*\\(");
                if (RE2Search(allBodies, redimPattern)) {
                    prop.isArray = true;
                    PLOGI.printf("ScriptPatcher: Detected '%s' as array via ReDim usage in class '%s'",
                                prop.name.c_str(), cls.name.c_str());
                }
            }
        }

        // Find IMPLICIT array declarations: ReDim varName( where varName is not an existing property
        // These are class-level variables created via ReDim in Class_Initialize
        static const RE2 redimAllPattern(R"((?i)\bReDim\s+(\w+)\s*\()");
        auto matches = RE2FindAll(allBodies, redimAllPattern);
        for (const auto& match : matches) {
            std::string varName = match.groups.size() > 0 ? match.groups[0] : "";
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
        }

        // Find IMPLICIT scalar properties: varName = value (assignment without declaration)
        // Look for patterns at statement boundaries: start of line, after colon, after Then/Else
        // These are class-level variables used without Public/Private declaration
        static const RE2 assignmentPattern(R"((?im)(^[ \t]*|:[ \t]*|\bThen[ \t]+|\bElse[ \t]+)([a-zA-Z_]\w*)\s*=\s*[^=])");
        auto assignMatches = RE2FindAll(allBodies, assignmentPattern);

        // Build set of method names and method parameters to exclude
        std::unordered_set<std::string> excludeNames;
        for (const auto& m : cls.methods) {
            std::string lowerMethod = m.name;
            std::transform(lowerMethod.begin(), lowerMethod.end(), lowerMethod.begin(), ::tolower);
            excludeNames.insert(lowerMethod);
            for (const auto& p : m.params) {
                std::string lowerParam = p;
                std::transform(lowerParam.begin(), lowerParam.end(), lowerParam.begin(), ::tolower);
                excludeNames.insert(lowerParam);
            }
        }
        for (const auto& a : cls.accessors) {
            for (const auto& p : a.params) {
                std::string lowerParam = p;
                std::transform(lowerParam.begin(), lowerParam.end(), lowerParam.begin(), ::tolower);
                excludeNames.insert(lowerParam);
            }
        }
        // Common VBScript/VPX keywords and objects to exclude
        static const std::unordered_set<std::string> vbsKeywords = {
            "i", "j", "k", "n", "x", "y", "z", "tmp", "temp", "result", "ret", "err",
            "lockwall", "lockpost", "movesword", "true", "false", "nothing", "empty", "null"
        };
        for (const auto& kw : vbsKeywords) excludeNames.insert(kw);

        for (const auto& match : assignMatches) {
            std::string varName = match.groups.size() > 1 ? match.groups[1] : "";
            std::string lowerName = varName;
            std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

            // Skip if already known or excluded
            if (existingProps.find(lowerName) != existingProps.end()) continue;
            if (excludeNames.find(lowerName) != excludeNames.end()) continue;

            // Add as implicit scalar property
            VBClassProperty implicitProp;
            implicitProp.name = varName;
            implicitProp.isPublic = false;
            implicitProp.isArray = false;
            implicitProp.arraySize = -1;
            cls.properties.push_back(implicitProp);
            existingProps.insert(lowerName);
            PLOGI.printf("ScriptPatcher: Added implicit scalar property '%s' in class '%s' (from assignment)",
                        varName.c_str(), cls.name.c_str());
        }
    }

    // Filter out classes that use vpmTimer.addResetObj Me - these pass 'Me' to external
    // code that expects a real VBScript object with callable methods, not a Dictionary
    static const RE2 addResetObjPattern(R"((?i)\bvpmTimer\s*\.\s*addResetObj\s+Me\b)");
    std::vector<VBClassDefinition> classesToEmulate;
    for (const auto& cls : classes) {
        std::string allBodies = cls.initializeBody + "\n";
        for (const auto& m : cls.methods) allBodies += m.body + "\n";

        if (RE2Search(allBodies, addResetObjPattern)) {
            PLOGI.printf("ScriptPatcher: Skipping emulation for '%s' (uses vpmTimer.addResetObj Me)",
                        cls.name.c_str());
        } else {
            classesToEmulate.push_back(cls);
        }
    }
    classes = classesToEmulate;

    PLOGI.printf("ScriptPatcher: Found %zu classes to emulate", classes.size());

    std::unordered_set<std::string> classNames;
    for (const auto& cls : classes) {
        classNames.insert(cls.name);
        PLOGI.printf("ScriptPatcher: Emulating '%s'", cls.name.c_str());
    }

    // Handle single-line stub classes (e.g., "Class X : ... : End Class" all on one line)
    // These are skipped by ParseClassDefinitions but need to be removed and stubbed
    // First pass: just collect class names and build stub emulation (don't modify script yet)
    std::ostringstream stubEmulation;
    std::vector<std::string> stubClassNames;  // Collect stub class names for TransformNewStatements
    static const RE2 singleLineClassRegex(R"((?im)^[ \t]*Class\s+(\w+)\s*:.+?End\s+Class[ \t]*$)");
    auto stubMatches = RE2FindAll(script, singleLineClassRegex);
    for (const auto& match : stubMatches) {
        std::string className = match.groups.size() > 0 ? match.groups[0] : "";
        PLOGI.printf("ScriptPatcher: Found single-line stub class '%s', creating minimal emulation", className.c_str());

        // Add to classNames for TransformNewStatements
        classNames.insert(className);
        stubClassNames.push_back(className);  // Save for later addition to classes

        // Create minimal stub emulation (just a factory function that returns empty Dictionary)
        stubEmulation << "' === " << className << " Stub Class Emulation ===\n";
        stubEmulation << "Function " << className << "_Create()\n";
        stubEmulation << "    Dim this_\n";
        stubEmulation << "    Set this_ = CreateObject(\"Scripting.Dictionary\")\n";
        stubEmulation << "    this_(\"__class__\") = \"" << className << "\"\n";
        stubEmulation << "    Set " << className << "_Create = this_\n";
        stubEmulation << "End Function\n\n";
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
    result = RE2Replace(result, singleLineClassRegex, "' [Stub class removed - emulated below]");

    // Add stub classes to classes vector for TransformNewStatements
    // (Must be AFTER EmitClassEmulation to avoid corrupting script at position 0)
    for (const auto& stubName : stubClassNames) {
        VBClassDefinition stubClass;
        stubClass.name = stubName;
        classes.push_back(stubClass);
    }

    // Inject stub emulation code if any single-line classes were found
    std::string stubCode = stubEmulation.str();
    if (!stubCode.empty()) {
        arrayDecls << stubCode;
    }

    // Inject array declarations at the start (after Option Explicit if present)
    static const RE2 optionExplicit(R"((?i)(Option\s+Explicit[^\r\n]*[\r\n]+))");
    RE2Match match;
    if (RE2FindFirst(result, optionExplicit, match)) {
        result = result.substr(0, match.position + match.length) + arrayDecls.str() + result.substr(match.position + match.length);
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
    std::unordered_map<std::string, std::string> varTypes; // varname -> className
    for (const auto& cls : classes) {
        // Pattern 1: Set varname = ClassName_Create()
        RE2 varAssignRegex1("(?i)Set\\s+(\\w+)\\s*=\\s*" + cls.name + "_Create\\s*\\(");
        auto matches1 = RE2FindAll(result, varAssignRegex1);
        for (const auto& m : matches1) {
            std::string varName = m.groups.size() > 0 ? m.groups[0] : "";
            std::string lowerVar = varName;
            std::transform(lowerVar.begin(), lowerVar.end(), lowerVar.begin(), ::tolower);
            varTypes[lowerVar] = cls.name;
        }

        // Pattern 2: Set varname = ClassName_init(ClassName_Create(), ...)
        // This handles classes with default methods (constructors with args)
        RE2 varAssignRegex2("(?i)Set\\s+(\\w+)\\s*=\\s*" + cls.name + "_\\w+\\s*\\(\\s*" + cls.name + "_Create\\s*\\(");
        auto matches2 = RE2FindAll(result, varAssignRegex2);
        for (const auto& m : matches2) {
            std::string varName = m.groups.size() > 0 ? m.groups[0] : "";
            std::string lowerVar = varName;
            std::transform(lowerVar.begin(), lowerVar.end(), lowerVar.begin(), ::tolower);
            varTypes[lowerVar] = cls.name;
        }
    }

    PLOGI.printf("ScriptPatcher: Found %zu typed variables", varTypes.size());
    for (const auto& [var, cls] : varTypes) {
        PLOGI.printf("ScriptPatcher:   varTypes[%s] = %s", var.c_str(), cls.c_str());
    }

    // Step 2: Find For Each loops and transform method calls on loop variables
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
    static const RE2 forEachArrayPattern(R"((?i)For\s+Each\s+(\w+)\s+In\s+Array\s*\(\s*(\w+)(?:\s*,\s*(\w+))*\s*\))");

    std::vector<std::tuple<size_t, size_t, std::string, std::string>> loopsToTransform;
    // tuple: (startPos, endPos, loopVar, className)

    auto forMatches = RE2FindAll(result, forEachArrayPattern);
    for (const auto& forMatch : forMatches) {
        std::string loopVar = forMatch.groups.size() > 0 ? forMatch.groups[0] : "";
        std::string firstArrayVar = forMatch.groups.size() > 1 ? forMatch.groups[1] : "";

        // Check if first array variable is a known typed variable
        std::string lowerFirstVar = firstArrayVar;
        std::transform(lowerFirstVar.begin(), lowerFirstVar.end(), lowerFirstVar.begin(), ::tolower);

        auto typeIt = varTypes.find(lowerFirstVar);
        if (typeIt != varTypes.end()) {
            std::string className = typeIt->second;
            size_t loopStart = forMatch.position + forMatch.length;

            // Find the matching Next
            static const RE2 nextPattern("(?i)\\bNext\\b");
            std::string afterLoop = result.substr(loopStart);
            RE2Match nextMatch;
            if (RE2FindFirst(afterLoop, nextPattern, nextMatch)) {
                size_t loopEnd = loopStart + nextMatch.position;
                loopsToTransform.push_back({loopStart, loopEnd, loopVar, className});
            }
        }
    }

    // For each For Each loop, find the NEAREST preceding Array() assignment
    static const RE2 forEachVarPattern(R"((?i)For\s+Each\s+(\w+)\s+In\s+(\w+))");
    auto forMatches2 = RE2FindAll(result, forEachVarPattern);
    for (const auto& forMatch : forMatches2) {
        std::string loopVar = forMatch.groups.size() > 0 ? forMatch.groups[0] : "";
        std::string arrayVar = forMatch.groups.size() > 1 ? forMatch.groups[1] : "";
        std::string lowerArrayVar = arrayVar;
        std::transform(lowerArrayVar.begin(), lowerArrayVar.end(), lowerArrayVar.begin(), ::tolower);

        // Skip if arrayVar is "Array" (handled by previous pattern)
        if (lowerArrayVar == "array") continue;

        size_t forEachPos = forMatch.position;

        // Look backwards for "arrayVar = Array(firstElem, ...)" before this For Each
        std::string beforeLoop = result.substr(0, forEachPos);
        RE2 arrayAssignPattern("(?i)" + arrayVar + "\\s*=\\s*Array\\s*\\(\\s*(\\w+)");
        auto arrayMatches = RE2FindAll(beforeLoop, arrayAssignPattern);

        if (!arrayMatches.empty()) {
            // Use the last match (nearest to the For Each)
            const auto& lastMatch = arrayMatches.back();
            std::string firstElem = lastMatch.groups.size() > 0 ? lastMatch.groups[0] : "";
            std::string lowerFirstElem = firstElem;
            std::transform(lowerFirstElem.begin(), lowerFirstElem.end(), lowerFirstElem.begin(), ::tolower);

            auto typeIt = varTypes.find(lowerFirstElem);
            if (typeIt != varTypes.end()) {
                std::string className = typeIt->second;
                size_t loopStart = forEachPos + forMatch.length;

                // Find the matching Next
                static const RE2 nextPattern("(?i)\\bNext\\b");
                std::string afterLoop = result.substr(loopStart);
                RE2Match nextMatch;
                if (RE2FindFirst(afterLoop, nextPattern, nextMatch)) {
                    size_t loopEnd = loopStart + nextMatch.position;
                    loopsToTransform.push_back({loopStart, loopEnd, loopVar, className});
                    PLOGI.printf("ScriptPatcher: Found For Each %s In %s (type %s at pos %zu)",
                                loopVar.c_str(), arrayVar.c_str(), className.c_str(), forEachPos);
                }
            }
        }
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
                    // RE2 doesn't support lookahead, capture trailing and check in callback
                    RE2 mr("(?i)\\b" + escapedLoopVar + "\\." + escapedMethod +
                           "\\b[ \\t]+([^:\\r\\n]+?)([ \\t]*(?::|\\r|\\n|$))");
                    std::string clsName = className;
                    std::string mName = m.name;
                    std::string lv = loopVar;
                    transformedBody = RE2ReplaceWithCallback(transformedBody, mr, [=](const RE2Match& match) -> std::string {
                        // Check what follows - if it starts with = or (, skip
                        std::string afterMethod = match.groups.size() > 0 ? match.groups[0] : "";
                        std::string trailing = match.groups.size() > 1 ? match.groups[1] : "";
                        std::string trimmed = afterMethod;
                        size_t start = trimmed.find_first_not_of(" \t");
                        if (start != std::string::npos && (trimmed[start] == '=' || trimmed[start] == '(')) {
                            return match.full_match;  // Keep original
                        }
                        return clsName + "_" + mName + " " + lv + ", " + afterMethod + trailing;
                    });
                } else {
                    // Method without params: loopVar.method
                    // RE2 doesn't support lookahead, capture trailing boundary and restore
                    RE2 mr("(?i)\\b" + escapedLoopVar + "\\." + escapedMethod +
                           "\\b([ \\t]*(?::|'|\\r|\\n|$))");
                    transformedBody = RE2Replace(transformedBody, mr,
                                                 className + "_" + m.name + " " + loopVar + "\\1");
                }
            }

            // Transform property access: loopVar.prop = value -> loopVar("prop") = value
            for (const auto& prop : cls.properties) {
                if (prop.isArray) continue;
                std::string escapedProp = EscapeRegex(prop.name);
                std::string escapedLoopVar = EscapeRegex(loopVar);

                // Assignment: loopVar.prop = value
                // RE2 doesn't support lookahead, capture trailing and restore
                RE2 pr("(?i)\\b" + escapedLoopVar + "\\." + escapedProp +
                       "\\s*=\\s*([^:\\r\\n]+?)([ \\t]*(?::|\\r|\\n|$))");
                transformedBody = RE2Replace(transformedBody, pr,
                                             loopVar + "(\"" + prop.name + "\") = \\1\\2");
            }
        }

        result = result.substr(0, startPos) + transformedBody + result.substr(endPos);
        PLOGI.printf("ScriptPatcher: Transformed For Each loop for %s (class %s)",
                     loopVar.c_str(), className.c_str());
    }

    // Step 3: Transform array element method/property access
    // Find arrays that contain emulated class instances
    std::unordered_map<std::string, std::string> arrayElementTypes; // arrayName -> className
    for (const auto& cls : classes) {
        // Pattern 1: Set arrayName(idx) = ClassName_Create()
        RE2 arrayAssignRegex1("(?i)Set\\s+(\\w+)\\s*\\([^)]+\\)\\s*=\\s*" + cls.name + "_Create\\s*\\(");
        auto matches1 = RE2FindAll(result, arrayAssignRegex1);
        for (const auto& m : matches1) {
            std::string arrayName = m.groups.size() > 0 ? m.groups[0] : "";
            std::string lowerArrayName = arrayName;
            std::transform(lowerArrayName.begin(), lowerArrayName.end(), lowerArrayName.begin(), ::tolower);
            arrayElementTypes[lowerArrayName] = cls.name;
            PLOGI.printf("ScriptPatcher: Array '%s' contains '%s' objects (pattern 1)", arrayName.c_str(), cls.name.c_str());
        }

        // Pattern 2: Set arrayName(idx) = ClassName_init(ClassName_Create(), ...)
        RE2 arrayAssignRegex2("(?i)Set\\s+(\\w+)\\s*\\([^)]+\\)\\s*=\\s*" + cls.name + "_\\w+\\s*\\(\\s*" + cls.name + "_Create\\s*\\(");
        auto matches2 = RE2FindAll(result, arrayAssignRegex2);
        for (const auto& m : matches2) {
            std::string arrayName = m.groups.size() > 0 ? m.groups[0] : "";
            std::string lowerArrayName = arrayName;
            std::transform(lowerArrayName.begin(), lowerArrayName.end(), lowerArrayName.begin(), ::tolower);
            arrayElementTypes[lowerArrayName] = cls.name;
            PLOGI.printf("ScriptPatcher: Array '%s' contains '%s' objects (pattern 2)", arrayName.c_str(), cls.name.c_str());
        }
    }

    // Pattern 3: ArrayName = Array(var1, var2, ...) where var1 is a known typed variable
    static const RE2 arrayLiteralPattern(R"((?i)(\w+)\s*=\s*Array\s*\(\s*(\w+))");
    auto matches3 = RE2FindAll(result, arrayLiteralPattern);
    for (const auto& m : matches3) {
        std::string arrayName = m.groups.size() > 0 ? m.groups[0] : "";
        std::string firstElem = m.groups.size() > 1 ? m.groups[1] : "";
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
    }

    PLOGI.printf("ScriptPatcher: Found %zu array element types", arrayElementTypes.size());
    for (const auto& [arr, cls] : arrayElementTypes) {
        PLOGI.printf("ScriptPatcher:   arrayElementTypes[%s] = %s", arr.c_str(), cls.c_str());
    }

    // Transform array element method calls and property access
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

            // IMPORTANT: Process patterns in order: paren-args, space-args, no-args
            // The no-args pattern would otherwise match everything and leave args dangling.

            // Pattern: arrayName(idx).method(args) (parenthesized args)
            // Transform: arrayName(idx).method(args) -> ClassName_method(arrayName(idx), args)
            // Use function call syntax for expression context compatibility
            RE2 parenArgsRegex("(?i)\\b(\\w+)\\s*\\(([^()]+)\\)\\." + escapedMethod + "\\s*\\(([^()]*)\\)");

            result = RE2ReplaceWithCallback(result, parenArgsRegex, [&](const RE2Match& match) -> std::string {
                std::string matchedArrayName = match.groups.size() > 0 ? match.groups[0] : "";
                std::string indexExpr = match.groups.size() > 1 ? match.groups[1] : "";
                std::string args = match.groups.size() > 2 ? match.groups[2] : "";

                std::string lowerMatched = matchedArrayName;
                std::transform(lowerMatched.begin(), lowerMatched.end(), lowerMatched.begin(), ::tolower);

                if (lowerMatched == lowerArrayName) {
                    PLOGI.printf("ScriptPatcher: Transformed %s(%s).%s(paren args) to method call",
                                matchedArrayName.c_str(), indexExpr.c_str(), m.name.c_str());
                    // Use function call syntax (parens) for expression context compatibility
                    if (args.empty()) {
                        return className + "_" + m.name + "(" + matchedArrayName + "(" + indexExpr + "))";
                    }
                    return className + "_" + m.name + "(" + matchedArrayName + "(" + indexExpr + "), " + args + ")";
                } else {
                    return match.full_match;
                }
            });

            // Pattern: arrayName(idx).method arg1, arg2, ... (space-separated args)
            // Transform: arrayName(idx).method args -> ClassName_method arrayName(idx), args
            // Match: word(expr).method followed by space and non-empty args until colon/newline
            // IMPORTANT: First char of args must NOT be whitespace, quote, equals, colon, or newline
            // Otherwise RE2 may match fewer spaces to capture whitespace as args start
            RE2 spaceArgsRegex("(?i)\\b(\\w+)\\s*\\(([^()]+)\\)\\." + escapedMethod + "[ \\t]+([^'=:\\r\\n\\s][^:\\r\\n]*)");

            result = RE2ReplaceWithCallback(result, spaceArgsRegex, [&](const RE2Match& match) -> std::string {
                std::string matchedArrayName = match.groups.size() > 0 ? match.groups[0] : "";
                std::string indexExpr = match.groups.size() > 1 ? match.groups[1] : "";
                std::string args = match.groups.size() > 2 ? match.groups[2] : "";

                std::string lowerMatched = matchedArrayName;
                std::transform(lowerMatched.begin(), lowerMatched.end(), lowerMatched.begin(), ::tolower);

                if (lowerMatched == lowerArrayName) {
                    // Trim trailing whitespace from args
                    size_t endPos = args.find_last_not_of(" \t\r\n");
                    if (endPos != std::string::npos) {
                        args = args.substr(0, endPos + 1);
                    } else {
                        args.clear();  // Args was all whitespace
                    }
                    // Transform: arrayName(idx).method args -> ClassName_method arrayName(idx), args
                    // Only add comma if args is not empty
                    if (args.empty()) {
                        PLOGI.printf("ScriptPatcher: Transformed %s(%s).%s (no args) to method call",
                                    matchedArrayName.c_str(), indexExpr.c_str(), m.name.c_str());
                        return className + "_" + m.name + " " + matchedArrayName + "(" + indexExpr + ")";
                    }
                    PLOGI.printf("ScriptPatcher: Transformed %s(%s).%s with args to method call",
                                matchedArrayName.c_str(), indexExpr.c_str(), m.name.c_str());
                    return className + "_" + m.name + " " + matchedArrayName + "(" + indexExpr + "), " + args;
                } else {
                    return match.full_match;
                }
            });

            // Pattern: arrayName(idx).method (no args, not followed by = or ()
            // RE2 doesn't support (?!), so we capture what follows and check in callback
            RE2 noArgsRegex("(?i)\\b(\\w+)\\s*\\(([^()]+)\\)\\." + escapedMethod + "\\b(\\s*[=(])?");

            result = RE2ReplaceWithCallback(result, noArgsRegex, [&](const RE2Match& match) -> std::string {
                // If group 3 matched (= or ( follows), keep original
                if (match.groups.size() > 2 && !match.groups[2].empty()) {
                    return match.full_match;  // Keep original - followed by = or (
                }

                std::string matchedArrayName = match.groups.size() > 0 ? match.groups[0] : "";
                std::string lowerMatched = matchedArrayName;
                std::transform(lowerMatched.begin(), lowerMatched.end(), lowerMatched.begin(), ::tolower);

                if (lowerMatched == lowerArrayName) {
                    // Transform: arrayName(idx).method -> ClassName_method arrayName(idx)
                    return className + "_" + m.name + " " + matchedArrayName + "(" + match.groups[1] + ")";
                } else {
                    // Not our array, keep original
                    return match.full_match;
                }
            });
        }

        // Build a set of property names (lowercase) for quick lookup
        std::unordered_set<std::string> propertyNames;
        std::unordered_map<std::string, std::string> propertyOriginalCase;
        for (const auto& prop : classDef->properties) {
            if (prop.isArray) continue;
            std::string lowerProp = prop.name;
            std::transform(lowerProp.begin(), lowerProp.end(), lowerProp.begin(), ::tolower);
            propertyNames.insert(lowerProp);
            propertyOriginalCase[lowerProp] = prop.name;
        }

        // Transform property access
        RE2 propRegex("(?i)\\b(\\w+)\\s*\\(([^()]+)\\)\\.(\\w+)\\b");
        result = RE2ReplaceWithCallback(result, propRegex, [&](const RE2Match& match) -> std::string {
            std::string matchedArrayName = match.groups.size() > 0 ? match.groups[0] : "";
            std::string indexExpr = match.groups.size() > 1 ? match.groups[1] : "";
            std::string propName = match.groups.size() > 2 ? match.groups[2] : "";

            std::string lowerMatched = matchedArrayName;
            std::transform(lowerMatched.begin(), lowerMatched.end(), lowerMatched.begin(), ::tolower);
            std::string lowerProp = propName;
            std::transform(lowerProp.begin(), lowerProp.end(), lowerProp.begin(), ::tolower);

            // Check if this is our array AND the property is from our class
            if (lowerMatched == lowerArrayName && propertyNames.count(lowerProp) > 0) {
                // Transform: arrayName(idx).prop -> arrayName(idx)("prop")
                std::string origProp = propertyOriginalCase[lowerProp];
                PLOGI.printf("ScriptPatcher: Transformed %s(%s).%s to dictionary access",
                            matchedArrayName.c_str(), indexExpr.c_str(), propName.c_str());
                return matchedArrayName + "(" + indexExpr + ")(\"" + origProp + "\")";
            } else {
                return match.full_match;
            }
        });

        // Transform accessor calls on array elements (Property Let/Get)
        // First pass: Let/Set accessors
        for (const auto& acc : classDef->accessors) {
            if (!EqualsIgnoreCase(acc.type, "Let") && !EqualsIgnoreCase(acc.type, "Set")) continue;
            std::string escapedAcc = EscapeRegex(acc.name);

            // Property Let WITH accessor params: arrayName(idx).accessor(param) = value -> ClassName_Let_accessor arrayName(idx), param, value
            // MUST be processed BEFORE the no-param pattern!
            RE2 letWithParamsRegex("(?im)(^[ \\t]*|:[ \\t]*|\\bThen[ \\t]+|\\bElse[ \\t]+)(\\w+)\\s*\\(([^()]+)\\)\\." + escapedAcc + "\\s*\\(([^()]*)\\)\\s*=\\s*([^:\\r\\n]+?)([ \\t]*(?::|\\r|\\n|$))");
            result = RE2ReplaceWithCallback(result, letWithParamsRegex, [&](const RE2Match& match) -> std::string {
                std::string prefix = match.groups.size() > 0 ? match.groups[0] : "";
                std::string matchedArrayName = match.groups.size() > 1 ? match.groups[1] : "";
                std::string indexExpr = match.groups.size() > 2 ? match.groups[2] : "";
                std::string accParams = match.groups.size() > 3 ? match.groups[3] : "";
                std::string value = match.groups.size() > 4 ? match.groups[4] : "";
                std::string trailing = match.groups.size() > 5 ? match.groups[5] : "";

                std::string lowerMatched = matchedArrayName;
                std::transform(lowerMatched.begin(), lowerMatched.end(), lowerMatched.begin(), ::tolower);

                if (lowerMatched == lowerArrayName) {
                    PLOGI.printf("ScriptPatcher: Transformed %s(%s).%s(%s) = to setter call with params",
                                matchedArrayName.c_str(), indexExpr.c_str(), acc.name.c_str(), accParams.c_str());
                    // Trim whitespace from accParams
                    size_t start = accParams.find_first_not_of(" \t");
                    size_t end = accParams.find_last_not_of(" \t");
                    if (start != std::string::npos) {
                        accParams = accParams.substr(start, end - start + 1);
                    } else {
                        accParams.clear();
                    }
                    if (accParams.empty()) {
                        return prefix + className + "_Let_" + acc.name + " " + matchedArrayName + "(" + indexExpr + "), " + value + trailing;
                    } else {
                        return prefix + className + "_Let_" + acc.name + " " + matchedArrayName + "(" + indexExpr + "), " + accParams + ", " + value + trailing;
                    }
                } else {
                    return match.full_match;
                }
            });

            // Property Let WITHOUT accessor params: arrayName(idx).accessor = value -> ClassName_Let_accessor arrayName(idx), value
            RE2 letRegex("(?im)(^[ \\t]*|:[ \\t]*|\\bThen[ \\t]+|\\bElse[ \\t]+)(\\w+)\\s*\\(([^()]+)\\)\\." + escapedAcc + "\\s*=\\s*([^:\\r\\n]+?)([ \\t]*(?::|\\r|\\n|$))");
            result = RE2ReplaceWithCallback(result, letRegex, [&](const RE2Match& match) -> std::string {
                std::string prefix = match.groups.size() > 0 ? match.groups[0] : "";
                std::string matchedArrayName = match.groups.size() > 1 ? match.groups[1] : "";
                std::string indexExpr = match.groups.size() > 2 ? match.groups[2] : "";
                std::string value = match.groups.size() > 3 ? match.groups[3] : "";
                std::string trailing = match.groups.size() > 4 ? match.groups[4] : "";

                std::string lowerMatched = matchedArrayName;
                std::transform(lowerMatched.begin(), lowerMatched.end(), lowerMatched.begin(), ::tolower);

                if (lowerMatched == lowerArrayName) {
                    return prefix + className + "_Let_" + acc.name + " " + matchedArrayName + "(" + indexExpr + "), " + value + trailing;
                } else {
                    return match.full_match;
                }
            });
        }

        // Second pass: Get accessors
        for (const auto& acc : classDef->accessors) {
            if (!EqualsIgnoreCase(acc.type, "Get")) continue;
            std::string escapedAcc = EscapeRegex(acc.name);

            // Property Get WITH params: arrayName(idx).accessor(params) -> ClassName_Get_accessor(arrayName(idx), params)
            // MUST be processed BEFORE the no-params pattern!
            RE2 getWithParamsRegex("(?i)\\b(\\w+)\\s*\\(([^()]+)\\)\\." + escapedAcc + "\\s*\\(([^()]*)\\)");
            result = RE2ReplaceWithCallback(result, getWithParamsRegex, [&](const RE2Match& match) -> std::string {
                std::string matchedArrayName = match.groups.size() > 0 ? match.groups[0] : "";
                std::string indexExpr = match.groups.size() > 1 ? match.groups[1] : "";
                std::string accParams = match.groups.size() > 2 ? match.groups[2] : "";

                std::string lowerMatched = matchedArrayName;
                std::transform(lowerMatched.begin(), lowerMatched.end(), lowerMatched.begin(), ::tolower);

                if (lowerMatched == lowerArrayName) {
                    PLOGI.printf("ScriptPatcher: Transformed %s(%s).%s(%s) to getter call with params",
                                matchedArrayName.c_str(), indexExpr.c_str(), acc.name.c_str(), accParams.c_str());
                    // Trim whitespace from accParams
                    size_t start = accParams.find_first_not_of(" \t");
                    size_t end = accParams.find_last_not_of(" \t");
                    if (start != std::string::npos) {
                        accParams = accParams.substr(start, end - start + 1);
                    } else {
                        accParams.clear();
                    }
                    if (accParams.empty()) {
                        return className + "_Get_" + acc.name + "(" + matchedArrayName + "(" + indexExpr + "))";
                    } else {
                        return className + "_Get_" + acc.name + "(" + matchedArrayName + "(" + indexExpr + "), " + accParams + ")";
                    }
                } else {
                    return match.full_match;
                }
            });

            // Property Get WITHOUT params: arrayName(idx).accessor -> ClassName_Get_accessor(arrayName(idx))
            RE2 getRegex("(?i)\\b(\\w+)\\s*\\(([^()]+)\\)\\." + escapedAcc + "\\b");
            result = RE2ReplaceWithCallback(result, getRegex, [&](const RE2Match& match) -> std::string {
                std::string matchedArrayName = match.groups.size() > 0 ? match.groups[0] : "";
                std::string indexExpr = match.groups.size() > 1 ? match.groups[1] : "";

                std::string lowerMatched = matchedArrayName;
                std::transform(lowerMatched.begin(), lowerMatched.end(), lowerMatched.begin(), ::tolower);

                if (lowerMatched == lowerArrayName) {
                    PLOGI.printf("ScriptPatcher: Transformed %s(%s).%s to getter call",
                                matchedArrayName.c_str(), indexExpr.c_str(), acc.name.c_str());
                    return className + "_Get_" + acc.name + "(" + matchedArrayName + "(" + indexExpr + "))";
                } else {
                    return match.full_match;
                }
            });
        }
    }

    // Step 4: Transform ExecuteGlobal string templates that contain dot notation
    for (const auto& cls : classes) {
        for (const auto& m : cls.methods) {
            // Pattern: & aName & ".methodName args
            RE2 dotRegex(R"RE((?i)"\s*&\s*(\w+)\s*&\s*"\.)RE" + EscapeRegex(m.name) + R"RE(\s+([^"]+))RE");
            result = RE2ReplaceWithCallback(result, dotRegex, [&](const RE2Match& match) -> std::string {
                std::string varName = match.groups.size() > 0 ? match.groups[0] : "";
                std::string args = match.groups.size() > 1 ? match.groups[1] : "";

                // Transform to: ClassName_methodName " & varName & ", args
                PLOGI.printf("ScriptPatcher: Transformed ExecuteGlobal template for %s.%s",
                            cls.name.c_str(), m.name.c_str());
                return cls.name + "_" + m.name + " \" & " + varName + " & \", " + args;
            });
        }
    }

    // ============================================================================
    // CHAINED ACCESSOR PATTERN FIX (MUST RUN LAST - after all other transforms!)
    // ============================================================================
    // Handle patterns like: cHouse_Get_BattleState(...).SetCompletedWithDireWolf = True
    // These occur when a Get accessor returns another class object, and we try to
    // access a Let accessor on that returned object.
    // Transform to: cBattleState_Let_SetCompletedWithDireWolf cHouse_Get_BattleState(...), True

    // Build map of Let/Set accessor names to their owning class
    std::unordered_map<std::string, std::string> letAccessorToClass;  // lowercase accName -> className
    for (const auto& cls : classes) {
        for (const auto& acc : cls.accessors) {
            if (EqualsIgnoreCase(acc.type, "Let") || EqualsIgnoreCase(acc.type, "Set")) {
                std::string lowerAcc = acc.name;
                std::transform(lowerAcc.begin(), lowerAcc.end(), lowerAcc.begin(), ::tolower);
                letAccessorToClass[lowerAcc] = cls.name;
            }
        }
    }

    // Build map of method names to their owning class
    std::unordered_map<std::string, std::string> chainedMethodMap;  // lowercase methodName -> className
    for (const auto& cls : classes) {
        for (const auto& method : cls.methods) {
            std::string lowerMethod = method.name;
            std::transform(lowerMethod.begin(), lowerMethod.end(), lowerMethod.begin(), ::tolower);
            chainedMethodMap[lowerMethod] = cls.name;
        }
    }

    // Find patterns: cXXX_Get_YYY(...).ZZZ = value where ... may have nested parens
    // Use [^()]* for paren matching to allow quotes inside (needed for ExecuteGlobal templates)
    RE2 chainedLetRegex(R"((?im)(^[ \t]*|:[ \t]*|\bThen[ \t]+|\bElse[ \t]+)(c\w+_Get_\w+\([^()]*(?:\([^()]*\)[^()]*)*\))\.(\w+)\s*=\s*([^:\r\n]+?)([ \t]*(?::|'|\r|\n|$)))");
    result = RE2ReplaceWithCallback(result, chainedLetRegex, [&](const RE2Match& m) -> std::string {
        std::string prefix = m.groups.size() > 0 ? m.groups[0] : "";
        std::string getterCall = m.groups.size() > 1 ? m.groups[1] : "";
        std::string chainedAcc = m.groups.size() > 2 ? m.groups[2] : "";
        std::string value = m.groups.size() > 3 ? m.groups[3] : "";
        std::string trailing = m.groups.size() > 4 ? m.groups[4] : "";

        std::string lowerChained = chainedAcc;
        std::transform(lowerChained.begin(), lowerChained.end(), lowerChained.begin(), ::tolower);

        auto it = letAccessorToClass.find(lowerChained);
        if (it != letAccessorToClass.end()) {
            const std::string& targetClass = it->second;
            PLOGI.printf("ScriptPatcher: Transformed chained Let accessor %s.%s to %s_Let_%s",
                        getterCall.c_str(), chainedAcc.c_str(), targetClass.c_str(), chainedAcc.c_str());
            return prefix + targetClass + "_Let_" + chainedAcc + " " + getterCall + ", " + value + trailing;
        }
        return m.full_match;
    });

    // Find patterns: cXXX_Get_YYY(...).MethodName args (space-separated args)
    // IMPORTANT: Use function call syntax (parens) not sub call syntax (space)
    // because the result may be used in expression context (after &, +, etc.)
    // - Paren matching uses [^()]* to allow quotes inside (needed for ExecuteGlobal templates)
    // - Args capture uses [^':\r\n\"]* to stop at comments (') and quotes (")
    RE2 chainedMethodArgsRegex(R"((?i)(c\w+_Get_\w+\([^()]*(?:\([^()]*\)[^()]*)*\))\.(\w+)[ \t]+([^'=:\r\n\s\"][^':\r\n\"]*))");
    result = RE2ReplaceWithCallback(result, chainedMethodArgsRegex, [&](const RE2Match& m) -> std::string {
        std::string getterCall = m.groups.size() > 0 ? m.groups[0] : "";
        std::string chainedMethod = m.groups.size() > 1 ? m.groups[1] : "";
        std::string args = m.groups.size() > 2 ? m.groups[2] : "";

        std::string lowerMethod = chainedMethod;
        std::transform(lowerMethod.begin(), lowerMethod.end(), lowerMethod.begin(), ::tolower);

        auto it = chainedMethodMap.find(lowerMethod);
        if (it != chainedMethodMap.end()) {
            const std::string& targetClass = it->second;
            size_t endPos = args.find_last_not_of(" \t\r\n");
            if (endPos != std::string::npos) {
                args = args.substr(0, endPos + 1);
            }
            PLOGI.printf("ScriptPatcher: Transformed chained method %s.%s to %s_%s",
                        getterCall.c_str(), chainedMethod.c_str(), targetClass.c_str(), chainedMethod.c_str());
            // Use function call syntax (parens) for expression context compatibility
            return targetClass + "_" + chainedMethod + "(" + getterCall + ", " + args + ")";
        }
        return m.full_match;
    });

    // Find patterns: cXXX_Get_YYY(...).MethodName (no args)
    // IMPORTANT: Use function call syntax (parens) for expression context compatibility
    // - Paren matching uses [^()]* to allow quotes inside (needed for ExecuteGlobal templates)
    RE2 chainedMethodNoArgsRegex(R"((?i)(c\w+_Get_\w+\([^()]*(?:\([^()]*\)[^()]*)*\))\.(\w+)\b)");
    result = RE2ReplaceWithCallback(result, chainedMethodNoArgsRegex, [&](const RE2Match& m) -> std::string {
        std::string getterCall = m.groups.size() > 0 ? m.groups[0] : "";
        std::string chainedMethod = m.groups.size() > 1 ? m.groups[1] : "";

        // Check what follows - skip if ( or = follows
        size_t afterPos = m.position + m.length;
        while (afterPos < result.length() && (result[afterPos] == ' ' || result[afterPos] == '\t')) {
            afterPos++;
        }
        if (afterPos < result.length() && (result[afterPos] == '(' || result[afterPos] == '=')) {
            return m.full_match;  // Let other patterns handle this
        }

        std::string lowerMethod = chainedMethod;
        std::transform(lowerMethod.begin(), lowerMethod.end(), lowerMethod.begin(), ::tolower);

        auto chainedIt = chainedMethodMap.find(lowerMethod);
        if (chainedIt != chainedMethodMap.end()) {
            const std::string& targetClass = chainedIt->second;
            PLOGI.printf("ScriptPatcher: Transformed chained method %s.%s (no args) to %s_%s",
                        getterCall.c_str(), chainedMethod.c_str(), targetClass.c_str(), chainedMethod.c_str());
            // Use function call syntax (parens) for expression context compatibility
            return targetClass + "_" + chainedMethod + "(" + getterCall + ")";
        }
        return m.full_match;
    });

    return result;
}

// ============================================================================
// EXISTING PATCHES
// ============================================================================


#endif // __STANDALONE__
