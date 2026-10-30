#pragma once

#include <windows.h>

#include <string>

namespace ExerciseUnlock::Calib
{
    // Developer option (/snapshot): a window as a .bmp, to check a layout
    // without a camera. Drawn straight into a bitmap - PrintWindow copies a
    // blank whenever the desktop isn't being composed (screen off or
    // locked): `paint(dc, client)` draws the window's own surface, then
    // each visible control prints itself on top.
    template <class Paint>
    void WriteWindowBmp(HWND hwnd, const std::wstring& path, Paint&& paint)
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC wdc = GetDC(hwnd);
        HDC dc = CreateCompatibleDC(wdc);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = rc.right;
        bi.bmiHeader.biHeight = rc.bottom;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(dc, bmp);
        paint(dc, rc);

        struct Target { HWND parent; HDC dc; } target{ hwnd, dc };
        EnumChildWindows(hwnd, [](HWND c, LPARAM lp) -> BOOL {
            const Target& t = *reinterpret_cast<const Target*>(lp);
            if (GetParent(c) != t.parent || !IsWindowVisible(c)) return TRUE;
            RECT cr;
            GetWindowRect(c, &cr);
            MapWindowPoints(nullptr, t.parent, reinterpret_cast<POINT*>(&cr), 2);
            POINT org{};
            SetViewportOrgEx(t.dc, cr.left, cr.top, &org);
            SendMessageW(c, WM_PRINT, reinterpret_cast<WPARAM>(t.dc), PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
            SetViewportOrgEx(t.dc, org.x, org.y, nullptr);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&target));

        GdiFlush();
        BITMAPFILEHEADER fh{};
        const DWORD imageBytes = static_cast<DWORD>(rc.right) * static_cast<DWORD>(rc.bottom) * 4;
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
        fh.bfSize = fh.bfOffBits + imageBytes;
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
            DWORD wrote = 0;
            WriteFile(f, &fh, sizeof(fh), &wrote, nullptr);
            WriteFile(f, &bi.bmiHeader, sizeof(bi.bmiHeader), &wrote, nullptr);
            WriteFile(f, bits, imageBytes, &wrote, nullptr);
            CloseHandle(f);
        }
        SelectObject(dc, old);
        DeleteObject(bmp);
        DeleteDC(dc);
        ReleaseDC(hwnd, wdc);
    }
}
