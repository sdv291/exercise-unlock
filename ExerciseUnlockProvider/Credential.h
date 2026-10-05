#pragma once

#include "SoundConfig.h"

#include <windows.h>
#include <credentialprovider.h>
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>

namespace ExerciseUnlock
{
    // v0.1 stub credential. It renders our tile on the sign-in screen
    // attached to a specific local user (by SID) and rejects any submit
    // attempt — the child must still use the real PIN/password tile for
    // now. Actual sign-in only starts coming in once the service +
    // exercise flow is in place.
    class Credential : public ICredentialProviderCredential2
    {
    public:
        Credential();

        HRESULT Initialize(const CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR* fields,
                           DWORD fieldCount,
                           PCWSTR userSid);

        // IUnknown
        IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
        IFACEMETHODIMP_(ULONG) AddRef() override;
        IFACEMETHODIMP_(ULONG) Release() override;

        // ICredentialProviderCredential
        IFACEMETHODIMP Advise(ICredentialProviderCredentialEvents* pcpce) override;
        IFACEMETHODIMP UnAdvise() override;
        IFACEMETHODIMP SetSelected(BOOL* pbAutoLogon) override;
        IFACEMETHODIMP SetDeselected() override;
        IFACEMETHODIMP GetFieldState(DWORD dwFieldID,
                                     CREDENTIAL_PROVIDER_FIELD_STATE* pcpfs,
                                     CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE* pcpfis) override;
        IFACEMETHODIMP GetStringValue(DWORD dwFieldID, PWSTR* ppsz) override;
        IFACEMETHODIMP GetBitmapValue(DWORD dwFieldID, HBITMAP* phbmp) override;
        IFACEMETHODIMP GetCheckboxValue(DWORD dwFieldID, BOOL* pbChecked, PWSTR* ppszLabel) override;
        IFACEMETHODIMP GetSubmitButtonValue(DWORD dwFieldID, DWORD* pdwAdjacentTo) override;
        IFACEMETHODIMP GetComboBoxValueCount(DWORD dwFieldID, DWORD* pcItems, DWORD* pdwSelectedItem) override;
        IFACEMETHODIMP GetComboBoxValueAt(DWORD dwFieldID, DWORD dwItem, PWSTR* ppsz) override;
        IFACEMETHODIMP SetStringValue(DWORD dwFieldID, PCWSTR psz) override;
        IFACEMETHODIMP SetCheckboxValue(DWORD dwFieldID, BOOL bChecked) override;
        IFACEMETHODIMP SetComboBoxSelectedValue(DWORD dwFieldID, DWORD dwSelectedItem) override;
        IFACEMETHODIMP CommandLinkClicked(DWORD dwFieldID) override;
        IFACEMETHODIMP GetSerialization(CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE* pcpgsr,
                                        CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* pcpcs,
                                        PWSTR* ppszOptionalStatusText,
                                        CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon) override;
        IFACEMETHODIMP ReportResult(NTSTATUS ntsStatus,
                                    NTSTATUS ntsSubstatus,
                                    PWSTR* ppszOptionalStatusText,
                                    CREDENTIAL_PROVIDER_STATUS_ICON* pcpsiOptionalStatusIcon) override;

        // ICredentialProviderCredential2
        IFACEMETHODIMP GetUserSid(PWSTR* ppsz) override;

    private:
        ~Credential();

        void StartPolling();
        void StopPolling();
        static VOID CALLBACK PollTimerCb(PVOID param, BOOLEAN timedOut);
        void RefreshStatus();

        LONG m_refCount;
        std::vector<CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR> m_fields;
        std::vector<std::wstring> m_values;
        std::wstring m_userSid;
        // Mutex guards m_events + m_values access from the poll timer
        // thread pool vs. the LogonUI thread that calls Advise/UnAdvise
        // and SetSelected/SetDeselected.
        std::mutex m_mx;
        ICredentialProviderCredentialEvents* m_events;
        HANDLE m_pollTimer;
        // Last posture-kind value seen. The provider plays a short
        // tone whenever this transitions None→Squat / None→PushUp —
        // the audible "detector sees you, start the rep now" cue.
        // UINT8_MAX = "not yet sampled" so the first poll doesn't
        // fire a spurious beep when the user walks up with the
        // camera already seeing them in a squat stance (we treat
        // UINT8_MAX as "was None" so the first observation of a
        // real posture still beeps — that's the fix for not
        // hearing anything when you were in position before
        // clicking the tile).
        uint8_t m_lastPostureKind{ UINT8_MAX };
        // Last rep-count values seen — a short "ding" fires on
        // every increment so the child hears each rep land. Two
        // different sounds vs. the start-cue (asterisk /
        // exclamation) keep "ready to go" and "rep counted"
        // audibly distinct. UINT32_MAX = "not yet sampled" so
        // persisted counters from a previous service run don't
        // dump a burst of beeps the first time we poll.
        uint32_t m_lastSquat{ UINT32_MAX };
        uint32_t m_lastPushup{ UINT32_MAX };
        // Sound configuration (library dir + per-event file lists).
        // Hot-reloaded on each RefreshStatus so edits to
        // %ProgramData%\ExerciseUnlock\sounds.ini take effect
        // without a sign-out.
        SoundConfig m_sounds;
    };
}
