// license:GPLv3+
// See ScriptWScriptShell.h for the rationale.

#include "ScriptWScriptShell.h"

#include <oaidl.h>
#include <oleauto.h>
#include <chrono>
#include <thread>
#include <cstring>

namespace {

// Stable DISPIDs. Scripts cache them via GetIDsOfNames the first time and
// then call Invoke with the cached id thereafter.
enum {
    DISPID_WSH_REGREAD              = 1,
    DISPID_WSH_REGWRITE             = 2,
    DISPID_WSH_REGDELETE            = 3,
    DISPID_WSH_RUN                  = 4,
    DISPID_WSH_EXEC                 = 5,
    DISPID_WSH_SLEEP                = 6,
    DISPID_WSH_WAIT                 = 7,
    DISPID_WSH_EXPANDENVSTRINGS     = 8,
    DISPID_WSH_ENVIRONMENT          = 9,
    DISPID_WSH_SPECIALFOLDERS       = 10,
    DISPID_WSH_CURRENTDIRECTORY     = 11,
    DISPID_WSH_APPACTIVATE          = 12,
    DISPID_WSH_POPUP                = 13,
    DISPID_WSH_SENDKEYS             = 14,
};

struct NameToDispid {
    const wchar_t* name;
    DISPID dispid;
};

// VBScript identifier lookup is case-insensitive — matched via _wcsicmp below.
static const NameToDispid kWshNames[] = {
    { L"RegRead",                  DISPID_WSH_REGREAD },
    { L"RegWrite",                 DISPID_WSH_REGWRITE },
    { L"RegDelete",                DISPID_WSH_REGDELETE },
    { L"Run",                      DISPID_WSH_RUN },
    { L"Exec",                     DISPID_WSH_EXEC },
    { L"Sleep",                    DISPID_WSH_SLEEP },
    { L"Wait",                     DISPID_WSH_WAIT },
    { L"ExpandEnvironmentStrings", DISPID_WSH_EXPANDENVSTRINGS },
    { L"Environment",              DISPID_WSH_ENVIRONMENT },
    { L"SpecialFolders",           DISPID_WSH_SPECIALFOLDERS },
    { L"CurrentDirectory",         DISPID_WSH_CURRENTDIRECTORY },
    { L"AppActivate",              DISPID_WSH_APPACTIVATE },
    { L"Popup",                    DISPID_WSH_POPUP },
    { L"SendKeys",                 DISPID_WSH_SENDKEYS },
};

class ScriptWScriptShell : public IDispatch
{
public:
    ScriptWScriptShell() : m_ref(1) {}
    virtual ~ScriptWScriptShell() = default;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDispatch)) {
            *ppv = static_cast<IDispatch*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }

    // IDispatch
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* pctinfo) override
    {
        if (!pctinfo) return E_POINTER;
        *pctinfo = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR* rgszNames,
                                            UINT cNames, LCID, DISPID* rgDispId) override
    {
        if (!rgszNames || !rgDispId) return E_POINTER;
        for (UINT i = 0; i < cNames; ++i) {
            DISPID dispid = DISPID_UNKNOWN;
            for (const NameToDispid& entry : kWshNames) {
                if (wcsicmp(rgszNames[i], entry.name) == 0) {
                    dispid = entry.dispid;
                    break;
                }
            }
            rgDispId[i] = dispid;
            if (dispid == DISPID_UNKNOWN)
                return DISP_E_UNKNOWNNAME;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Invoke(DISPID dispIdMember, REFIID, LCID, WORD wFlags,
                                     DISPPARAMS* pDispParams, VARIANT* pVarResult,
                                     EXCEPINFO*, UINT*) override
    {
        const unsigned argCount = pDispParams ? pDispParams->cArgs : 0;
        // VBScript dispatch passes args in REVERSE order — arg(0) is the FIRST
        // logical positional, arg(n-1) is the last.
        auto arg = [&](unsigned i) -> VARIANT* {
            return &pDispParams->rgvarg[argCount - 1 - i];
        };

        switch (dispIdMember)
        {
        case DISPID_WSH_REGREAD: {
            // RegRead(name) -> Empty (key not found is the natural Android default)
            if (pVarResult) {
                VariantInit(pVarResult);
                V_VT(pVarResult) = VT_EMPTY;
            }
            return S_OK;
        }

        case DISPID_WSH_REGWRITE:
        case DISPID_WSH_REGDELETE:
        case DISPID_WSH_SENDKEYS:
            // Pure side-effect methods — silent no-op.
            if (pVarResult) {
                VariantInit(pVarResult);
                V_VT(pVarResult) = VT_EMPTY;
            }
            return S_OK;

        case DISPID_WSH_RUN: {
            // Run(cmd, [show], [wait]) -> exit code 0 (success).
            // Real implementation isn't possible on Android (no shell exec).
            if (pVarResult) {
                VariantInit(pVarResult);
                V_VT(pVarResult) = VT_I4;
                V_I4(pVarResult) = 0;
            }
            return S_OK;
        }

        case DISPID_WSH_EXEC: {
            // Exec returns a WshScriptExec object. We don't have one — return
            // Empty and let the script's `If x.Status = 1` etc. fall through.
            if (pVarResult) {
                VariantInit(pVarResult);
                V_VT(pVarResult) = VT_EMPTY;
            }
            return S_OK;
        }

        case DISPID_WSH_SLEEP:
        case DISPID_WSH_WAIT: {
            // Sleep(milliseconds) — real delay using std::this_thread::sleep_for.
            // Many tables use this for animation timing; making it real (vs. no-op)
            // keeps frame pacing closer to author intent.
            if (argCount >= 1) {
                LONG ms = 0;
                VARIANT v;
                VariantInit(&v);
                if (SUCCEEDED(VariantChangeType(&v, arg(0), 0, VT_I4))) {
                    ms = V_I4(&v);
                }
                VariantClear(&v);
                if (ms > 0 && ms < 60000) {  // clamp to 60s for safety
                    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                }
            }
            if (pVarResult) {
                VariantInit(pVarResult);
                V_VT(pVarResult) = VT_EMPTY;
            }
            return S_OK;
        }

        case DISPID_WSH_EXPANDENVSTRINGS: {
            // Pass the input through unchanged. Tables use this to resolve
            // `%USERPROFILE%` etc. — on Android we have no env, so the literal
            // is the safest fallback (script's downstream file-existence check
            // will fail naturally if it actually needs the resolved path).
            if (!pVarResult) return E_POINTER;
            VariantInit(pVarResult);
            if (argCount >= 1) {
                VARIANT v;
                VariantInit(&v);
                HRESULT hr = VariantChangeType(&v, arg(0), 0, VT_BSTR);
                if (SUCCEEDED(hr)) {
                    V_VT(pVarResult) = VT_BSTR;
                    V_BSTR(pVarResult) = SysAllocString(V_BSTR(&v) ? V_BSTR(&v) : L"");
                }
                VariantClear(&v);
                if (V_VT(pVarResult) == VT_BSTR && V_BSTR(pVarResult))
                    return S_OK;
            }
            V_VT(pVarResult) = VT_BSTR;
            V_BSTR(pVarResult) = SysAllocString(L"");
            return S_OK;
        }

        case DISPID_WSH_ENVIRONMENT: {
            // Environment(scope) returns a WshEnvironment collection. Return
            // Empty — `WshShell.Environment("USER")("name")` will then yield
            // an OBJECT_REQUIRED error, but most scripts wrap this in
            // `On Error Resume Next`. If a table needs real env access we
            // can extend this to return a stub collection.
            if (pVarResult) {
                VariantInit(pVarResult);
                V_VT(pVarResult) = VT_EMPTY;
            }
            return S_OK;
        }

        case DISPID_WSH_SPECIALFOLDERS: {
            // SpecialFolders("AllUsersDesktop") etc. -> "" (best-effort fallback).
            if (!pVarResult) return E_POINTER;
            VariantInit(pVarResult);
            V_VT(pVarResult) = VT_BSTR;
            V_BSTR(pVarResult) = SysAllocString(L"");
            return S_OK;
        }

        case DISPID_WSH_CURRENTDIRECTORY: {
            // Read returns "", write is silently swallowed.
            if (wFlags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)) {
                if (pVarResult) {
                    VariantInit(pVarResult);
                    V_VT(pVarResult) = VT_EMPTY;
                }
                return S_OK;
            }
            if (!pVarResult) return E_POINTER;
            VariantInit(pVarResult);
            V_VT(pVarResult) = VT_BSTR;
            V_BSTR(pVarResult) = SysAllocString(L"");
            return S_OK;
        }

        case DISPID_WSH_APPACTIVATE: {
            // AppActivate(title) -> False (we never bring foreign windows to front)
            if (!pVarResult) return E_POINTER;
            VariantInit(pVarResult);
            V_VT(pVarResult) = VT_BOOL;
            V_BOOL(pVarResult) = VARIANT_FALSE;
            return S_OK;
        }

        case DISPID_WSH_POPUP: {
            // Popup(text, ...) -> 0 (button code for "no UI shown")
            if (!pVarResult) return E_POINTER;
            VariantInit(pVarResult);
            V_VT(pVarResult) = VT_I4;
            V_I4(pVarResult) = 0;
            return S_OK;
        }

        default:
            return DISP_E_MEMBERNOTFOUND;
        }
    }

private:
    ULONG m_ref;
};

}  // namespace

IDispatch* CreateScriptWScriptShell()
{
    return new (std::nothrow) ScriptWScriptShell();
}
