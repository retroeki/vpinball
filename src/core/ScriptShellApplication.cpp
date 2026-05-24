// license:GPLv3+
// See ScriptShellApplication.h for the rationale.

#include "ScriptShellApplication.h"

#include <oaidl.h>
#include <oleauto.h>
#include <cstring>
#include <new>

namespace {

// Stable DISPIDs for the methods/properties scripts touch. VBScript caches
// them via GetIDsOfNames the first time and then calls Invoke with the
// cached id thereafter.
enum {
   DISPID_SHELL_NAMESPACE          = 1,
   DISPID_SHELL_OPEN               = 2,
   DISPID_SHELL_EXPLORE            = 3,
   DISPID_SHELL_SHELLEXECUTE       = 4,
   DISPID_SHELL_WINDOWS            = 5,
   DISPID_SHELL_BROWSEFORFOLDER    = 6,
   DISPID_SHELL_MINIMIZEALL        = 7,
   DISPID_SHELL_UNDOMINIMIZEALL    = 8,
   DISPID_SHELL_TILEVERTICALLY     = 9,
   DISPID_SHELL_TILEHORIZONTALLY   = 10,
   DISPID_SHELL_CASCADEWINDOWS     = 11,
   DISPID_SHELL_CONTROLPANELITEM   = 12,
   DISPID_SHELL_TRAYPROPERTIES     = 13,
   DISPID_SHELL_HELP               = 14,
   DISPID_SHELL_FINDFILES          = 15,
   DISPID_SHELL_SUSPEND            = 16,
   DISPID_SHELL_EJECTPC            = 17,
   DISPID_SHELL_SETTIME            = 18,
   DISPID_SHELL_APPLICATION        = 19,
   DISPID_SHELL_PARENT             = 20,
};

struct NameToDispid {
   const wchar_t* name;
   DISPID dispid;
};

// VBScript identifier lookup is case-insensitive — matched via _wcsicmp below.
static const NameToDispid kShellNames[] = {
   { L"NameSpace",         DISPID_SHELL_NAMESPACE },
   { L"Open",              DISPID_SHELL_OPEN },
   { L"Explore",           DISPID_SHELL_EXPLORE },
   { L"ShellExecute",      DISPID_SHELL_SHELLEXECUTE },
   { L"Windows",           DISPID_SHELL_WINDOWS },
   { L"BrowseForFolder",   DISPID_SHELL_BROWSEFORFOLDER },
   { L"MinimizeAll",       DISPID_SHELL_MINIMIZEALL },
   { L"UndoMinimizeAll",   DISPID_SHELL_UNDOMINIMIZEALL },
   { L"TileVertically",    DISPID_SHELL_TILEVERTICALLY },
   { L"TileHorizontally",  DISPID_SHELL_TILEHORIZONTALLY },
   { L"CascadeWindows",    DISPID_SHELL_CASCADEWINDOWS },
   { L"ControlPanelItem",  DISPID_SHELL_CONTROLPANELITEM },
   { L"TrayProperties",    DISPID_SHELL_TRAYPROPERTIES },
   { L"Help",              DISPID_SHELL_HELP },
   { L"FindFiles",         DISPID_SHELL_FINDFILES },
   { L"Suspend",           DISPID_SHELL_SUSPEND },
   { L"EjectPC",           DISPID_SHELL_EJECTPC },
   { L"SetTime",           DISPID_SHELL_SETTIME },
   { L"Application",       DISPID_SHELL_APPLICATION },
   { L"Parent",            DISPID_SHELL_PARENT },
};

class ScriptShellApplication : public IDispatch
{
public:
   ScriptShellApplication() : m_ref(1) {}
   virtual ~ScriptShellApplication() = default;

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
         for (const NameToDispid& entry : kShellNames) {
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

   HRESULT STDMETHODCALLTYPE Invoke(DISPID dispIdMember, REFIID, LCID, WORD,
                                    DISPPARAMS*, VARIANT* pVarResult,
                                    EXCEPINFO*, UINT*) override
   {
      // Every method on shell.application is either:
      //   - A folder/window enumeration that we can't service on Android
      //     (return Empty so the caller's `if (not x is nothing)` triggers)
      //   - A user-action method (Open/Explore/ShellExecute/Suspend/etc.)
      //     that has no meaningful Android equivalent (silent no-op)
      //
      // We don't distinguish between these — both want Empty / no-op. The
      // only "data" methods (Application, Parent) point at the COM object
      // itself; returning Empty is safe because nothing in the script
      // ecosystem chains off them on Android.
      switch (dispIdMember)
      {
      case DISPID_SHELL_NAMESPACE:
      case DISPID_SHELL_OPEN:
      case DISPID_SHELL_EXPLORE:
      case DISPID_SHELL_SHELLEXECUTE:
      case DISPID_SHELL_WINDOWS:
      case DISPID_SHELL_BROWSEFORFOLDER:
      case DISPID_SHELL_MINIMIZEALL:
      case DISPID_SHELL_UNDOMINIMIZEALL:
      case DISPID_SHELL_TILEVERTICALLY:
      case DISPID_SHELL_TILEHORIZONTALLY:
      case DISPID_SHELL_CASCADEWINDOWS:
      case DISPID_SHELL_CONTROLPANELITEM:
      case DISPID_SHELL_TRAYPROPERTIES:
      case DISPID_SHELL_HELP:
      case DISPID_SHELL_FINDFILES:
      case DISPID_SHELL_SUSPEND:
      case DISPID_SHELL_EJECTPC:
      case DISPID_SHELL_SETTIME:
      case DISPID_SHELL_APPLICATION:
      case DISPID_SHELL_PARENT:
         if (pVarResult) {
            VariantInit(pVarResult);
            V_VT(pVarResult) = VT_EMPTY;
         }
         return S_OK;

      default:
         return DISP_E_MEMBERNOTFOUND;
      }
   }

private:
   ULONG m_ref;
};

}  // namespace

IDispatch* CreateScriptShellApplication()
{
   return new (std::nothrow) ScriptShellApplication();
}
