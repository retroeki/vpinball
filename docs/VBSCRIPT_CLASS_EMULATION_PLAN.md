# VBScript Class Emulation for Wine

## Status: IMPLEMENTED

Implementation completed on 2026-01-04 in:
- `src/ui/scriptpatcher.h` - Data structures and declarations
- `src/ui/scriptpatcher.cpp` - Full implementation (~675 lines)

---

## Problem Statement

Wine's VBScript implementation doesn't support the `Class` keyword. Tables using VPW physics classes (SlingshotCorrection, FlipperPolarity, etc.) fail to load on Android/Wine.

## Solution: Dictionary-Based Class Emulation

Transform VBScript classes into Wine-compatible Dictionary objects at the C++ level in ScriptPatcher before Wine parses the script.

---

## How It Works

### Original VBScript:
```vbscript
Class SlingshotCorrection
    Public Enabled
    Private Slingshot

    Private Sub Class_Initialize()
        Enabled = True
    End Sub

    Public Sub Init(obj)
        Set Slingshot = obj
    End Sub

    Public Function IsActive()
        IsActive = Enabled
    End Function
End Class

Set sc = New SlingshotCorrection
sc.Init SwL
x = sc.IsActive()
```

### Transformed (Wine-Compatible):
```vbscript
' === SlingshotCorrection Class Emulation ===
Function SlingshotCorrection_Create()
    Dim this_
    Set this_ = CreateObject("Scripting.Dictionary")
    this_("__class__") = "SlingshotCorrection"
    this_("Enabled") = Empty
    this_("Slingshot") = Empty
    ' Class_Initialize
    this_("Enabled") = True
    Set SlingshotCorrection_Create = this_
End Function

Sub SlingshotCorrection_Init(this_, obj)
    Set this_("Slingshot") = obj
End Sub

Function SlingshotCorrection_IsActive(this_)
    SlingshotCorrection_IsActive = this_("Enabled")
End Function
' === End SlingshotCorrection ===

Set sc = SlingshotCorrection_Create()
SlingshotCorrection_Init sc, SwL
x = SlingshotCorrection_IsActive(sc)
```

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                      ScriptPatcher::PatchScript()                 │
│                                                                   │
│  1. HasClassDefinitions() - Quick check for "Class " keyword     │
│                                ↓                                  │
│  2. ParseClassDefinitions() - Extract class structure            │
│     • Properties (Public/Private variables)                       │
│     • Methods (Sub/Function with bodies)                          │
│     • Accessors (Property Get/Let/Set)                           │
│     • Class_Initialize/Terminate bodies                          │
│                                ↓                                  │
│  3. EmitClassEmulation() - Generate Dictionary-based code        │
│     • ClassName_Create() factory function                        │
│     • ClassName_MethodName(this_, ...) global functions          │
│     • TransformMethodBody() for Me.X → this_("X")               │
│                                ↓                                  │
│  4. TransformNewStatements() - Set x = New Class → Create()      │
│                                ↓                                  │
│  5. TransformMethodCalls() - x.Method() → Class_Method x         │
│     • Tracks variable types from Set x = Class_Create()          │
│                                ↓                                  │
│  6. TransformPropertyAccess() - x.Prop → x("Prop")              │
└──────────────────────────────────────────────────────────────────┘
```

---

## Data Structures

```cpp
struct VBClassProperty {
    std::string name;
    bool isPublic;
};

struct VBClassMethod {
    std::string name;
    bool isPublic;
    bool isFunction;    // true = Function, false = Sub
    bool isDefault;     // "Public Default Function"
    std::vector<std::string> params;
    std::string body;
};

struct VBClassAccessor {
    std::string name;
    std::string type;   // "Get", "Let", or "Set"
    std::vector<std::string> params;
    std::string body;
};

struct VBClassDefinition {
    std::string name;
    std::vector<VBClassProperty> properties;
    std::vector<VBClassMethod> methods;
    std::vector<VBClassAccessor> accessors;
    std::string initializeBody;
    std::string terminateBody;
    size_t startPos;
    size_t endPos;
};
```

---

## Key Transformations

| Original | Transformed |
|----------|-------------|
| `Class X ... End Class` | `Function X_Create() ... End Function` + methods |
| `Set x = New ClassName` | `Set x = ClassName_Create()` |
| `x.Method(args)` | `ClassName_Method x, args` |
| `x.Method` | `ClassName_Method x` |
| `x.Property` | `x("Property")` |
| `x.Property = val` | `x("Property") = val` |
| `Me.Property` (in method) | `this_("Property")` |
| `Me` (in method) | `this_` |

---

## Limitations

1. **Type Tracking**: Only tracks types for simple `Set x = ClassName_Create()` patterns. Complex expressions or function parameters aren't tracked.

2. **Nested Object Access**: `x.Child.Method()` may not transform correctly.

3. **Method Chaining**: `x.Method1().Method2()` not supported.

4. **With Blocks**: `.Property` inside `With obj` needs manual handling.

5. **Class_Terminate**: No destructor equivalent - cleanup must be explicit.

---

## Files Modified

- `src/ui/scriptpatcher.h` - Added data structures and function declarations
- `src/ui/scriptpatcher.cpp` - Full implementation (~675 lines)

---

## Testing

Tables to test:
1. **LOTR Valinor Edition** - Uses SlingshotCorrection, FlipperPolarity
2. **Other VPW tables** - Various physics classes
3. **Tables without classes** - Ensure no regression

---

*Implementation completed: 2026-01-04*
*For: Wine VBScript Class Emulation in VPinball Android*
