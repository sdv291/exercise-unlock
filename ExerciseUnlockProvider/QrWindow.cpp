#include "QrWindow.h"
#include "Log.h"
#include "Paths.h"
#include "Protocol.h"

#include <algorithm>
#include <iterator>

extern HINSTANCE g_hInstance;
extern LONG      g_dllRefCount;

namespace ExerciseUnlock
{
    namespace
    {
        constexpr wchar_t kClassName[] = L"ExerciseUnlockPhoneQr";
        constexpr UINT    WM_APP_APPLY = WM_APP + 1;
        constexpr int     kModulePx    = 8;     // at 96 dpi; 29 modules -> ~300 px
        constexpr int     kQuiet       = 4;     // white border required around a QR code

        // config.ini lives next to the data folder (<InstallDir>\data).
        std::wstring ConfigIniPath()
        {
            std::wstring dir = DataDir();
            const size_t slash = dir.rfind(L'\\');
            if (slash != std::wstring::npos) dir.resize(slash);
            return dir + L"\\config.ini";
        }

        // [labels] phone_* in config.ini, else built-in text in the
        // system UI language (Ukrainian / Russian -> Ukrainian).
        std::wstring Label(const wchar_t* key, const wchar_t* uk, const wchar_t* en)
        {
            const LANGID lang = PRIMARYLANGID(GetUserDefaultUILanguage());
            const wchar_t* dflt = (lang == LANG_UKRAINIAN || lang == LANG_RUSSIAN) ? uk : en;
            wchar_t buf[256]{};
            GetPrivateProfileStringW(L"labels", key, dflt, buf, static_cast<DWORD>(std::size(buf)),
                                     ConfigIniPath().c_str());
            return buf;
        }
    }

    QrWindow::~QrWindow()
    {
        Destroy();
    }

    void QrWindow::Create(HWND owner)
    {
        if (m_hwnd || !owner) return;

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc   = &QrWindow::WndProc;
        wc.hInstance     = g_hInstance;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            LogF(L"QrWindow: RegisterClass failed %lu", GetLastError());
            return;
        }

        m_textScan          = Label(L"phone_scan", L"Відскануй телефоном", L"Scan with your phone");
        m_textScanHint      = Label(L"phone_scan_hint", L"і роби вправи перед камерою телефона",
                                    L"and exercise in front of the phone camera");
        m_textConnected     = Label(L"phone_connected", L"Телефон підключено", L"Phone connected");
        m_textConnectedHint = Label(L"phone_connected_hint", L"Роби вправи - повтори зараховуються",
                                    L"Exercise - your reps are being counted");

        m_owner = owner;
        // Owned, topmost, never activated: LogonUI keeps the focus.
        m_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                 kClassName, L"", WS_POPUP, 0, 0, 10, 10,
                                 owner, nullptr, g_hInstance, this);
        if (!m_hwnd)
        {
            LogF(L"QrWindow: CreateWindow failed %lu", GetLastError());
            UnregisterClassW(kClassName, g_hInstance);
            return;
        }
        InterlockedIncrement(&g_dllRefCount);
        m_dpi = GetDpiForWindow(m_hwnd);
        if (m_dpi == 0) m_dpi = 96;
        auto font = [this](int pt, int weight) {
            return CreateFontW(-MulDiv(pt, static_cast<int>(m_dpi), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI");
        };
        m_fontTitle = font(16, FW_SEMIBOLD);
        m_fontText  = font(11, FW_NORMAL);
        m_shownUrl.clear();
        LogF(L"QrWindow: created");
        Apply();
    }

    void QrWindow::Destroy()
    {
        if (!m_hwnd) return;
        HWND hwnd = m_hwnd;
        m_hwnd = nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);   // stray messages: no `this`
        DestroyWindow(hwnd);
        UnregisterClassW(kClassName, g_hInstance);
        if (m_fontTitle) { DeleteObject(m_fontTitle); m_fontTitle = nullptr; }
        if (m_fontText)  { DeleteObject(m_fontText);  m_fontText = nullptr; }
        InterlockedDecrement(&g_dllRefCount);
        LogF(L"QrWindow: destroyed");
    }

    void QrWindow::SetContent(uint8_t phoneState, const std::wstring& url)
    {
        {
            std::lock_guard<std::mutex> lk(m_mx);
            if (phoneState == m_state && url == m_url) return;
            m_state = phoneState;
            m_url = url;
        }
        if (HWND hwnd = m_hwnd) PostMessageW(hwnd, WM_APP_APPLY, 0, 0);
    }

    void QrWindow::Apply()
    {
        if (!m_hwnd) return;
        uint8_t state;
        std::wstring url;
        {
            std::lock_guard<std::mutex> lk(m_mx);
            state = m_state;
            url = m_url;
        }
        const bool waiting   = state == static_cast<uint8_t>(Ipc::PhoneState::Waiting) && !url.empty();
        const bool connected = state == static_cast<uint8_t>(Ipc::PhoneState::Connected);
        m_shownState = state;
        if (!waiting && !connected)
        {
            ShowWindow(m_hwnd, SW_HIDE);
            return;
        }
        if (waiting && url != m_shownUrl)
        {
            // The URL is plain ASCII (upper-case, base32 token).
            std::string text;
            for (const wchar_t c : url) text += (c < 128) ? static_cast<char>(c) : '?';
            m_code = Qr::Code{};
            if (!Qr::Encode(text, m_code))
            {
                LogF(L"QrWindow: cannot encode '%s'", url.c_str());
                ShowWindow(m_hwnd, SW_HIDE);
                return;
            }
            m_shownUrl = url;
        }
        Layout();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    }

    // Right of the centred tile, vertically centred on LogonUI's monitor.
    void QrWindow::Layout()
    {
        const bool connected = m_shownState == static_cast<uint8_t>(Ipc::PhoneState::Connected);
        const int qrPx  = (m_code.size + 2 * kQuiet) * Px(kModulePx);
        const int pad   = Px(20);
        const int width = (std::max)(qrPx, Px(300)) + 2 * pad;
        const int height = connected ? pad + Px(150) + pad
                                     : pad + Px(36) + qrPx + Px(44) + pad;

        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(m_owner, MONITOR_DEFAULTTOPRIMARY), &mi);
        const RECT& r = mi.rcMonitor;
        const int x = r.right - width - (r.right - r.left) / 12;
        const int y = r.top + ((r.bottom - r.top) - height) / 2;
        SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
        SetWindowRgn(m_hwnd, CreateRoundRectRgn(0, 0, width + 1, height + 1, Px(20), Px(20)), TRUE);
    }

    void QrWindow::Paint(HDC dc, const RECT& rc)
    {
        HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(dc, &rc, white);
        DeleteObject(white);
        SetBkMode(dc, TRANSPARENT);
        const int pad = Px(20);

        if (m_shownState == static_cast<uint8_t>(Ipc::PhoneState::Connected))
        {
            // Green check + text instead of the code.
            const int d = Px(56), cx = (rc.right - d) / 2, cy = pad + Px(4);
            HBRUSH green = CreateSolidBrush(RGB(30, 170, 100));
            HGDIOBJ oldBrush = SelectObject(dc, green);
            HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, cx, cy, cx + d, cy + d);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
            DeleteObject(green);
            HPEN tick = CreatePen(PS_SOLID, Px(6), RGB(255, 255, 255));
            oldPen = SelectObject(dc, tick);
            MoveToEx(dc, cx + d * 27 / 100, cy + d * 52 / 100, nullptr);
            LineTo(dc, cx + d * 44 / 100, cy + d * 68 / 100);
            LineTo(dc, cx + d * 74 / 100, cy + d * 34 / 100);
            SelectObject(dc, oldPen);
            DeleteObject(tick);

            RECT t{ pad, cy + d + Px(10), rc.right - pad, cy + d + Px(46) };
            SelectObject(dc, m_fontTitle);
            SetTextColor(dc, RGB(31, 37, 71));
            DrawTextW(dc, m_textConnected.c_str(), -1, &t, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            RECT h{ pad, t.bottom, rc.right - pad, rc.bottom - pad / 2 };
            SelectObject(dc, m_fontText);
            SetTextColor(dc, RGB(90, 96, 130));
            DrawTextW(dc, m_textConnectedHint.c_str(), -1, &h, DT_CENTER | DT_WORDBREAK);
            return;
        }

        RECT t{ pad, pad, rc.right - pad, pad + Px(32) };
        SelectObject(dc, m_fontTitle);
        SetTextColor(dc, RGB(31, 37, 71));
        DrawTextW(dc, m_textScan.c_str(), -1, &t, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        const int m = Px(kModulePx);
        const int qrPx = (m_code.size + 2 * kQuiet) * m;
        const int ox = (rc.right - qrPx) / 2 + kQuiet * m;
        const int oy = pad + Px(36) + kQuiet * m;
        HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
        for (int y = 0; y < m_code.size; ++y)
            for (int x = 0; x < m_code.size; ++x)
                if (m_code.At(x, y))
                {
                    const RECT cell{ ox + x * m, oy + y * m, ox + (x + 1) * m, oy + (y + 1) * m };
                    FillRect(dc, &cell, black);
                }
        DeleteObject(black);

        RECT h{ pad, oy + (m_code.size + kQuiet) * m, rc.right - pad, rc.bottom - pad / 2 };
        SelectObject(dc, m_fontText);
        SetTextColor(dc, RGB(90, 96, 130));
        DrawTextW(dc, m_textScanHint.c_str(), -1, &h, DT_CENTER | DT_WORDBREAK);
    }

    LRESULT CALLBACK QrWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NCCREATE)
        {
            const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = reinterpret_cast<QrWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg)
        {
        case WM_APP_APPLY:
            if (self) self->Apply();
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC wdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (self && rc.right > 0 && rc.bottom > 0)
            {
                // Double-buffered: no flicker when the code changes.
                HDC dc = CreateCompatibleDC(wdc);
                HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
                HGDIOBJ old = SelectObject(dc, bmp);
                self->Paint(dc, rc);
                BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
                SelectObject(dc, old);
                DeleteObject(bmp);
                DeleteDC(dc);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }
}
