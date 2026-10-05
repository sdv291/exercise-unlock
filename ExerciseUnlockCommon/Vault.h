#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace ExerciseUnlock::Vault
{
    // What the parent has to hand over once per child: enough for the
    // credential provider to build a KERB_INTERACTIVE_UNLOCK_LOGON
    // blob at unlock time. Empty `domain` means "." (local machine).
    struct Credentials
    {
        std::wstring domain;
        std::wstring username;
        std::wstring password;
    };

    // C:\ProgramData\ExerciseUnlock\vault.dat by default. Created on
    // first Store() with an ACL that only SYSTEM and the local
    // Administrators group can read/write.
    std::wstring VaultPath();

    // Upsert one credential (identified by case-insensitive username)
    // into the vault. On-disk format is a list of entries encrypted
    // together via DPAPI (LocalMachine scope).
    HRESULT Store(const Credentials& creds);

    // Load the entry whose username matches (case-insensitive). Zeros
    // `out` on any failure. Returns HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
    // when the vault exists but has no such user.
    HRESULT Load(const std::wstring& username, Credentials& out);

    // Legacy convenience: load "the first" entry. Useful for callers
    // that don't know the username up front (or single-child setups).
    HRESULT Load(Credentials& out);

    // All entries (username / domain only — passwords also filled).
    HRESULT LoadAll(std::vector<Credentials>& out);

    // Delete one entry. Returns S_OK if it wasn't there anyway.
    HRESULT Remove(const std::wstring& username);

    bool    Exists();                       // any entries at all
    HRESULT Clear();                        // wipe the vault entirely
}
