#pragma once

#include "QrCode.h"

#include <windows.h>
#include <cstdint>
#include <mutex>
#include <string>

namespace ExerciseUnlock
{
    // Popup beside the lock-screen tile with the phone pairing QR code
    // (the service's PhoneLink provides the URL). On Windows 10/11 the big
    // picture above the tile is always the account picture and the tile
    // itself only holds text, so the code gets a window of its own.
    //
    // Create / Destroy run on LogonUI's UI thread (SetSelected /
    // SetDeselected); SetContent may come from the poll thread. While the
    // window exists it holds a DLL reference, so LogonUI can't unload the
    // provider under it.
    class QrWindow
    {
    public:
        ~QrWindow();

        void Create(HWND owner);
        void Destroy();
        // phoneState: Ipc::PhoneState. Shown only while Waiting (QR) or
        // Connected ("phone connected"); hidden otherwise.
        void SetContent(uint8_t phoneState, const std::wstring& url);

    private:
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
        void Apply();
        void Layout();
        void Paint(HDC dc, const RECT& rc);
        int  Px(int v) const { return MulDiv(v, static_cast<int>(m_dpi), 96); }

        HWND         m_hwnd  = nullptr;
        HWND         m_owner = nullptr;
        UINT         m_dpi   = 96;
        HFONT        m_fontTitle = nullptr;
        HFONT        m_fontText  = nullptr;

        std::mutex   m_mx;              // guards m_state / m_url
        uint8_t      m_state = 0;
        std::wstring m_url;

        uint8_t      m_shownState = 0;  // UI thread only below
        std::wstring m_shownUrl;
        Qr::Code     m_code;
        std::wstring m_textScan, m_textScanHint, m_textConnected, m_textConnectedHint;
    };
}
