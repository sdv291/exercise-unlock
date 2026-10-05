#include <windows.h>
#include <unknwn.h>
#include <new>

#include "guids.h"
#include "ClassFactory.h"
#include "Log.h"

// Tracks live objects + explicit LockServer holds. LogonUI polls
// DllCanUnloadNow to decide whether it can unload us between sign-in
// screens, so this must be exact.
LONG g_dllRefCount = 0;

// Our own module handle, captured at load time. Needed to LoadImage the
// embedded tile bitmap out of this DLL rather than out of LogonUI.exe.
HINSTANCE g_hInstance = nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*lpReserved*/)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hInstance = hModule;
        DisableThreadLibraryCalls(hModule);
        ExerciseUnlock::LogF(L"DllMain: DLL_PROCESS_ATTACH");
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        ExerciseUnlock::LogF(L"DllMain: DLL_PROCESS_DETACH");
    }
    return TRUE;
}

extern "C" HRESULT __stdcall DllCanUnloadNow()
{
    return (g_dllRefCount == 0) ? S_OK : S_FALSE;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    if (!ppv)
    {
        return E_POINTER;
    }
    *ppv = nullptr;

    if (rclsid != CLSID_ExerciseUnlockProvider)
    {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    ExerciseUnlock::LogF(L"DllGetClassObject: matched CLSID_ExerciseUnlockProvider");

    auto* factory = new (std::nothrow) ExerciseUnlock::ClassFactory();
    if (!factory)
    {
        return E_OUTOFMEMORY;
    }
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}
