// license:GPLv3+
// See ScriptArrayList.h for the rationale.

#include "ScriptArrayList.h"

#include <oaidl.h>
#include <oleauto.h>
#include <vector>
#include <algorithm>
#include <cstring>

namespace {

// DISPIDs for the ArrayList interface. Stable values — scripts cache them via
// GetIDsOfNames the first time and call Invoke with the cached id thereafter.
enum {
    DISPID_AL_VALUE     = DISPID_VALUE,    // 0 — default property = .Item
    DISPID_AL_NEWENUM   = DISPID_NEWENUM,  // -4 — For Each support
    DISPID_AL_ADD       = 1,
    DISPID_AL_ITEM      = 2,
    DISPID_AL_COUNT     = 3,
    DISPID_AL_CLEAR     = 4,
    DISPID_AL_CONTAINS  = 5,
    DISPID_AL_INDEXOF   = 6,
    DISPID_AL_REMOVE    = 7,
    DISPID_AL_REMOVEAT  = 8,
    DISPID_AL_INSERT    = 9,
    DISPID_AL_SORT      = 10,
    DISPID_AL_REVERSE   = 11,
    DISPID_AL_TOARRAY   = 12,
    DISPID_AL_CLONE     = 13,
};

struct NameToDispid {
    const wchar_t* name;
    DISPID dispid;
};

// Names exposed via GetIDsOfNames. VBScript identifier matching is
// case-insensitive (handled below via _wcsicmp).
static const NameToDispid kArrayListNames[] = {
    { L"Add",       DISPID_AL_ADD },
    { L"Item",      DISPID_AL_ITEM },
    { L"Count",     DISPID_AL_COUNT },
    { L"Clear",     DISPID_AL_CLEAR },
    { L"Contains",  DISPID_AL_CONTAINS },
    { L"IndexOf",   DISPID_AL_INDEXOF },
    { L"Remove",    DISPID_AL_REMOVE },
    { L"RemoveAt",  DISPID_AL_REMOVEAT },
    { L"Insert",    DISPID_AL_INSERT },
    { L"Sort",      DISPID_AL_SORT },
    { L"Reverse",   DISPID_AL_REVERSE },
    { L"ToArray",   DISPID_AL_TOARRAY },
    { L"Clone",     DISPID_AL_CLONE },
};

// ----------------------------------------------------------------------------
// Variant equality used by Contains / IndexOf / Remove. We coerce numeric vs
// string comparison to match what VBScript's `=` operator does, since
// scripts will say `list.IndexOf(49)` when they previously did `list.Add 49`
// and the value may have round-tripped through different VARTYPEs.
// ----------------------------------------------------------------------------
static bool VariantEquals(const VARIANT& a, const VARIANT& b)
{
    HRESULT hr = VarCmp(const_cast<VARIANT*>(&a), const_cast<VARIANT*>(&b),
                        LOCALE_USER_DEFAULT, 0);
    return hr == VARCMP_EQ;
}

// VarCmp wrapper for sort: returns negative / 0 / positive.
static int VariantOrder(const VARIANT& a, const VARIANT& b)
{
    HRESULT hr = VarCmp(const_cast<VARIANT*>(&a), const_cast<VARIANT*>(&b),
                        LOCALE_USER_DEFAULT, 0);
    if (hr == VARCMP_LT) return -1;
    if (hr == VARCMP_GT) return 1;
    return 0;  // EQ or NULL
}

// ----------------------------------------------------------------------------
// Forward declarations
// ----------------------------------------------------------------------------
class ScriptArrayList;
class ArrayListEnum;

// ----------------------------------------------------------------------------
// IEnumVARIANT impl returned by DISPID_NEWENUM so VBScript `For Each` works.
// ----------------------------------------------------------------------------
class ArrayListEnum : public IEnumVARIANT
{
public:
    ArrayListEnum(const std::vector<VARIANT>& source)
        : m_ref(1), m_pos(0)
    {
        m_items.reserve(source.size());
        for (const VARIANT& v : source) {
            VARIANT copy;
            VariantInit(&copy);
            VariantCopy(&copy, const_cast<VARIANT*>(&v));
            m_items.push_back(copy);
        }
    }

    ~ArrayListEnum()
    {
        for (VARIANT& v : m_items)
            VariantClear(&v);
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumVARIANT)) {
            *ppv = static_cast<IEnumVARIANT*>(this);
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

    // IEnumVARIANT
    HRESULT STDMETHODCALLTYPE Next(ULONG celt, VARIANT* rgVar, ULONG* pCeltFetched) override
    {
        if (!rgVar) return E_POINTER;
        ULONG fetched = 0;
        while (fetched < celt && m_pos < m_items.size()) {
            VariantInit(&rgVar[fetched]);
            VariantCopy(&rgVar[fetched], &m_items[m_pos]);
            ++m_pos;
            ++fetched;
        }
        if (pCeltFetched) *pCeltFetched = fetched;
        return (fetched == celt) ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Skip(ULONG celt) override
    {
        ULONG remaining = (ULONG)m_items.size() - m_pos;
        if (celt > remaining) {
            m_pos = (ULONG)m_items.size();
            return S_FALSE;
        }
        m_pos += celt;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Reset() override
    {
        m_pos = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IEnumVARIANT** ppEnum) override
    {
        if (!ppEnum) return E_POINTER;
        ArrayListEnum* clone = new ArrayListEnum(m_items);
        clone->m_pos = m_pos;
        *ppEnum = clone;
        return S_OK;
    }

private:
    ULONG m_ref;
    ULONG m_pos;
    std::vector<VARIANT> m_items;
};

// ----------------------------------------------------------------------------
// IDispatch impl backing System.Collections.ArrayList.
// ----------------------------------------------------------------------------
class ScriptArrayList : public IDispatch
{
public:
    ScriptArrayList() : m_ref(1) {}

    ~ScriptArrayList()
    {
        Clear_internal();
    }

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
            for (const NameToDispid& entry : kArrayListNames) {
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
        // VBScript dispatch passes args in REVERSE order, so arg[0] is the
        // LAST positional. Helper to fetch by logical index:
        auto arg = [&](unsigned i) -> VARIANT* {
            return &pDispParams->rgvarg[argCount - 1 - i];
        };

        switch (dispIdMember)
        {
        case DISPID_AL_NEWENUM:
            if (!pVarResult) return E_POINTER;
            VariantInit(pVarResult);
            V_VT(pVarResult) = VT_UNKNOWN;
            V_UNKNOWN(pVarResult) = new ArrayListEnum(m_items);
            return S_OK;

        case DISPID_AL_ADD: {
            if (argCount < 1) return DISP_E_BADPARAMCOUNT;
            VARIANT copy;
            VariantInit(&copy);
            HRESULT hr = VariantCopy(&copy, arg(0));
            if (FAILED(hr)) return hr;
            m_items.push_back(copy);
            if (pVarResult) {
                VariantInit(pVarResult);
                V_VT(pVarResult) = VT_I4;
                V_I4(pVarResult) = (LONG)m_items.size() - 1;
            }
            return S_OK;
        }

        case DISPID_AL_VALUE:    // .Item via default
        case DISPID_AL_ITEM: {
            if (wFlags & (DISPATCH_PROPERTYGET | DISPATCH_METHOD)) {
                if (argCount < 1) return DISP_E_BADPARAMCOUNT;
                LONG idx;
                HRESULT hr = VariantToInt(arg(0), &idx);
                if (FAILED(hr)) return hr;
                if (idx < 0 || (size_t)idx >= m_items.size())
                    return DISP_E_BADINDEX;
                if (!pVarResult) return E_POINTER;
                VariantInit(pVarResult);
                return VariantCopy(pVarResult, &m_items[idx]);
            }
            if (wFlags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)) {
                if (argCount < 2) return DISP_E_BADPARAMCOUNT;
                // PUT params: rgvarg[0] = value, rgvarg[1] = index
                LONG idx;
                HRESULT hr = VariantToInt(arg(0), &idx);  // index is "first" logical arg
                if (FAILED(hr)) return hr;
                if (idx < 0 || (size_t)idx >= m_items.size())
                    return DISP_E_BADINDEX;
                VariantClear(&m_items[idx]);
                return VariantCopy(&m_items[idx], arg(1));
            }
            return DISP_E_MEMBERNOTFOUND;
        }

        case DISPID_AL_COUNT: {
            if (!pVarResult) return E_POINTER;
            VariantInit(pVarResult);
            V_VT(pVarResult) = VT_I4;
            V_I4(pVarResult) = (LONG)m_items.size();
            return S_OK;
        }

        case DISPID_AL_CLEAR:
            Clear_internal();
            return S_OK;

        case DISPID_AL_CONTAINS: {
            if (argCount < 1) return DISP_E_BADPARAMCOUNT;
            bool found = false;
            for (const VARIANT& v : m_items) {
                if (VariantEquals(v, *arg(0))) { found = true; break; }
            }
            if (pVarResult) {
                VariantInit(pVarResult);
                V_VT(pVarResult) = VT_BOOL;
                V_BOOL(pVarResult) = found ? VARIANT_TRUE : VARIANT_FALSE;
            }
            return S_OK;
        }

        case DISPID_AL_INDEXOF: {
            if (argCount < 1) return DISP_E_BADPARAMCOUNT;
            LONG idx = -1;
            for (size_t i = 0; i < m_items.size(); ++i) {
                if (VariantEquals(m_items[i], *arg(0))) { idx = (LONG)i; break; }
            }
            if (pVarResult) {
                VariantInit(pVarResult);
                V_VT(pVarResult) = VT_I4;
                V_I4(pVarResult) = idx;
            }
            return S_OK;
        }

        case DISPID_AL_REMOVE: {
            if (argCount < 1) return DISP_E_BADPARAMCOUNT;
            for (auto it = m_items.begin(); it != m_items.end(); ++it) {
                if (VariantEquals(*it, *arg(0))) {
                    VariantClear(&*it);
                    m_items.erase(it);
                    break;
                }
            }
            return S_OK;
        }

        case DISPID_AL_REMOVEAT: {
            if (argCount < 1) return DISP_E_BADPARAMCOUNT;
            LONG idx;
            HRESULT hr = VariantToInt(arg(0), &idx);
            if (FAILED(hr)) return hr;
            if (idx < 0 || (size_t)idx >= m_items.size())
                return DISP_E_BADINDEX;
            VariantClear(&m_items[idx]);
            m_items.erase(m_items.begin() + idx);
            return S_OK;
        }

        case DISPID_AL_INSERT: {
            if (argCount < 2) return DISP_E_BADPARAMCOUNT;
            LONG idx;
            HRESULT hr = VariantToInt(arg(0), &idx);
            if (FAILED(hr)) return hr;
            if (idx < 0 || (size_t)idx > m_items.size())
                return DISP_E_BADINDEX;
            VARIANT copy;
            VariantInit(&copy);
            hr = VariantCopy(&copy, arg(1));
            if (FAILED(hr)) return hr;
            m_items.insert(m_items.begin() + idx, copy);
            return S_OK;
        }

        case DISPID_AL_SORT: {
            std::sort(m_items.begin(), m_items.end(),
                [](const VARIANT& a, const VARIANT& b) {
                    return VariantOrder(a, b) < 0;
                });
            return S_OK;
        }

        case DISPID_AL_REVERSE:
            std::reverse(m_items.begin(), m_items.end());
            return S_OK;

        case DISPID_AL_TOARRAY: {
            if (!pVarResult) return E_POINTER;
            SAFEARRAY* sa = SafeArrayCreateVector(VT_VARIANT, 0, (ULONG)m_items.size());
            if (!sa) return E_OUTOFMEMORY;
            VARIANT* dest = nullptr;
            SafeArrayAccessData(sa, (void**)&dest);
            for (size_t i = 0; i < m_items.size(); ++i) {
                VariantInit(&dest[i]);
                VariantCopy(&dest[i], &m_items[i]);
            }
            SafeArrayUnaccessData(sa);
            VariantInit(pVarResult);
            V_VT(pVarResult) = VT_ARRAY | VT_VARIANT;
            V_ARRAY(pVarResult) = sa;
            return S_OK;
        }

        case DISPID_AL_CLONE: {
            if (!pVarResult) return E_POINTER;
            ScriptArrayList* clone = new ScriptArrayList();
            for (const VARIANT& v : m_items) {
                VARIANT copy;
                VariantInit(&copy);
                VariantCopy(&copy, const_cast<VARIANT*>(&v));
                clone->m_items.push_back(copy);
            }
            VariantInit(pVarResult);
            V_VT(pVarResult) = VT_DISPATCH;
            V_DISPATCH(pVarResult) = clone;
            return S_OK;
        }

        default:
            return DISP_E_MEMBERNOTFOUND;
        }
    }

private:
    void Clear_internal()
    {
        for (VARIANT& v : m_items)
            VariantClear(&v);
        m_items.clear();
    }

    static HRESULT VariantToInt(VARIANT* v, LONG* out)
    {
        VARIANT temp;
        VariantInit(&temp);
        HRESULT hr = VariantChangeType(&temp, v, 0, VT_I4);
        if (SUCCEEDED(hr)) *out = V_I4(&temp);
        VariantClear(&temp);
        return hr;
    }

    ULONG m_ref;
    std::vector<VARIANT> m_items;
};

}  // namespace

IDispatch* CreateScriptArrayList()
{
    return new ScriptArrayList();
}
