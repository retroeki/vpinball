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
 */

#include "stdafx.h"

#ifdef __STANDALONE__

#include "scriptpatcher.h"
#include "scriptpatcher_internal.h"
#include <regex>
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

    // Filter out classes that use vpmTimer.addResetObj Me - these pass 'Me' to external
    // code that expects a real VBScript object with callable methods, not a Dictionary
    std::regex addResetObjPattern(R"(\bvpmTimer\s*\.\s*addResetObj\s+Me\b)", std::regex::icase);
    std::vector<VBClassDefinition> classesToEmulate;
    for (const auto& cls : classes) {
        std::string allBodies = cls.initializeBody + "\n";
        for (const auto& m : cls.methods) allBodies += m.body + "\n";

        if (std::regex_search(allBodies, addResetObjPattern)) {
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
    std::regex singleLineClassRegex(R"(^[ \t]*Class\s+(\w+)\s*:.+?End\s+Class[ \t]*$)", std::regex::icase | std::regex::multiline);
    std::smatch singleMatch;
    std::string searchStr = script;
    while (std::regex_search(searchStr, singleMatch, singleLineClassRegex)) {
        std::string className = singleMatch[1].str();
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
