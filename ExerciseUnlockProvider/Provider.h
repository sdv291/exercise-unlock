#pragma once

#include <windows.h>
#include <credentialprovider.h>
#include <vector>

namespace ExerciseUnlock
{
    class Credential;

    // ICredentialProvider is the top-level entry point Windows LogonUI
    // calls into. It advertises how many "tiles" we contribute to the
    // sign-in screen and hands them out on request. For v0.1 we enumerate
    // local users and contribute one Exercise Unlock tile per user, so
    // our tile appears alongside Password/PIN for each account.
    class Provider : public ICredentialProvider
    {
    public:
        Provider();

        IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
        IFACEMETHODIMP_(ULONG) AddRef() override;
        IFACEMETHODIMP_(ULONG) Release() override;

        IFACEMETHODIMP SetUsageScenario(CREDENTIAL_PROVIDER_USAGE_SCENARIO cpus, DWORD dwFlags) override;
        IFACEMETHODIMP SetSerialization(const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs) override;
        IFACEMETHODIMP Advise(ICredentialProviderEvents* pcpe, UINT_PTR upAdviseContext) override;
        IFACEMETHODIMP UnAdvise() override;
        IFACEMETHODIMP GetFieldDescriptorCount(DWORD* pdwCount) override;
        IFACEMETHODIMP GetFieldDescriptorAt(DWORD dwIndex, CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR** ppcpfd) override;
        IFACEMETHODIMP GetCredentialCount(DWORD* pdwCount, DWORD* pdwDefault, BOOL* pbAutoLogonWithDefault) override;
        IFACEMETHODIMP GetCredentialAt(DWORD dwIndex, ICredentialProviderCredential** ppcpc) override;

    private:
        ~Provider();

        void ReleaseCredentials();
        HRESULT CreateCredentialsForLocalUsers();

        LONG m_refCount;
        CREDENTIAL_PROVIDER_USAGE_SCENARIO m_cpus;
        std::vector<Credential*> m_credentials;
    };
}
