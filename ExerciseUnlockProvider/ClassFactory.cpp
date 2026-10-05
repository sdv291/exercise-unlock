#include "ClassFactory.h"
#include "Provider.h"
#include "Log.h"

#include <new>

extern LONG g_dllRefCount;

namespace ExerciseUnlock
{
    ClassFactory::ClassFactory() : m_refCount(1)
    {
        InterlockedIncrement(&g_dllRefCount);
    }

    IFACEMETHODIMP ClassFactory::QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv)
        {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IClassFactory)
        {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) ClassFactory::AddRef()
    {
        return InterlockedIncrement(&m_refCount);
    }

    IFACEMETHODIMP_(ULONG) ClassFactory::Release()
    {
        LONG n = InterlockedDecrement(&m_refCount);
        if (n == 0)
        {
            InterlockedDecrement(&g_dllRefCount);
            delete this;
        }
        return static_cast<ULONG>(n);
    }

    IFACEMETHODIMP ClassFactory::CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv)
    {
        if (!ppv)
        {
            return E_POINTER;
        }
        *ppv = nullptr;
        if (pUnkOuter != nullptr)
        {
            return CLASS_E_NOAGGREGATION;
        }

        LogF(L"ClassFactory::CreateInstance");

        Provider* provider = new (std::nothrow) Provider();
        if (!provider)
        {
            return E_OUTOFMEMORY;
        }

        HRESULT hr = provider->QueryInterface(riid, ppv);
        provider->Release();
        return hr;
    }

    IFACEMETHODIMP ClassFactory::LockServer(BOOL fLock)
    {
        if (fLock)
        {
            InterlockedIncrement(&g_dllRefCount);
        }
        else
        {
            InterlockedDecrement(&g_dllRefCount);
        }
        return S_OK;
    }
}
