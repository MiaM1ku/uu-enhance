#include <windows.h>
#include <shellapi.h>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "app.h"
#include "config.h"
#include "log.h"
#include "session.h"
#include "update.h"

#define WM_TRAY         (WM_APP + 17)
#define WM_UPDATE_FOUND (WM_APP + 18)
enum { ID_GITHUB = 1, ID_UPDATE_DL = 4, ID_UPDATE_NOW = 5, ID_AUTO_UPDATE = 6 };

static HWND  g_wnd = nullptr;
static NOTIFYICONDATAW g_nid{};

static void show_menu(HWND hwnd) {
    POINT pt; GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();

    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"被控期间仍可远控其他主机");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    {
        DebugInfo dbg = debug_snapshot();
        HMENU sub = CreatePopupMenu();
        wchar_t line[256];
        swprintf_s(line, L"GameViewer %ls (%ls)", dbg.gvVersion.c_str(), dbg.gvKnown ? L"known" : L"unknown");
        AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, line);
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);

        std::vector<const DbgLine*> items;
        for (const auto& h : dbg.hooks) items.push_back(&h);
        std::sort(items.begin(), items.end(), [](const DbgLine* a, const DbgLine* b) {
            if (a->ok != b->ok) return !a->ok;
            return _stricmp(a->name.c_str(), b->name.c_str()) < 0;
        });
        int ok = 0;
        for (const DbgLine* h : items) {
            if (h->ok) ++ok;
            if (!h->ok)      swprintf_s(line, L"%hs  not found", h->name.c_str());
            else if (h->off) swprintf_s(line, L"%hs  %hs +0x%llX", h->name.c_str(), h->how.c_str(), h->off);
            else             swprintf_s(line, L"%hs  %hs", h->name.c_str(), h->how.c_str());
            AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, line);
        }
        wchar_t glabel[64];
        swprintf_s(glabel, L"调试信息（%d/%d）", ok, (int)items.size());
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, glabel);
    }

    if (update::available()) {
        wchar_t l[128];
        swprintf_s(l, L"发现新版本 v%ls（前往下载）", update::latest_version().c_str());
        AppendMenuW(m, MF_STRING, ID_UPDATE_DL, l);
    }
    AppendMenuW(m, MF_STRING, ID_UPDATE_NOW, L"立即检查更新");
    AppendMenuW(m, MF_STRING | (cfg::g_autoUpdate.load() ? MF_CHECKED : 0),
                ID_AUTO_UPDATE, L"自动检查更新");
    AppendMenuW(m, MF_STRING, ID_GITHUB, L"项目主页 (GitHub)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"UU远程增强 v" UURE_VERSION_W);

    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);

    if (cmd == ID_GITHUB) {
        ShellExecuteW(nullptr, L"open", UURE_GITHUB_W, nullptr, nullptr, SW_SHOWNORMAL);
    } else if (cmd == ID_UPDATE_DL) {
        ShellExecuteW(nullptr, L"open", UURE_GITHUB_W L"/releases/latest", nullptr, nullptr, SW_SHOWNORMAL);
    } else if (cmd == ID_UPDATE_NOW) {
        update::check_async();
    } else if (cmd == ID_AUTO_UPDATE) {
        cfg::g_autoUpdate = !cfg::g_autoUpdate.load();
        cfg::save();
        if (cfg::g_autoUpdate.load()) update::check_async();
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_TRAY) {
        if (l == WM_RBUTTONUP || l == WM_LBUTTONUP || l == WM_CONTEXTMENU) show_menu(h);
        else if (l == NIN_BALLOONUSERCLICK && update::available())
            ShellExecuteW(nullptr, L"open", UURE_GITHUB_W L"/releases/latest", nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
    }
    if (msg == WM_UPDATE_FOUND) {
        std::wstring v = update::latest_version();
        g_nid.uFlags = NIF_INFO;
        wcscpy_s(g_nid.szInfoTitle, L"UU远程增强 · 有新版本");
        swprintf_s(g_nid.szInfo, L"发现新版本 v%ls，点此前往下载；或右键图标 → 更新。", v.c_str());
        g_nid.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

static HICON overlayGear(HICON base) {
    int cx = GetSystemMetrics(SM_CXSMICON);
    int cy = GetSystemMetrics(SM_CYSMICON);
    int n = cx * cy;

    HDC screen = GetDC(nullptr);
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih); bih.biWidth = cx; bih.biHeight = -cy;
    bih.biPlanes = 1; bih.biBitCount = 32; bih.biCompression = BI_RGB;

    HDC dc = CreateCompatibleDC(screen);
    DWORD* px = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, (BITMAPINFO*)&bih, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    std::memset(px, 0, n * 4);
    DrawIconEx(dc, 0, 0, base, cx, cy, 0, nullptr, DI_NORMAL);
    SelectObject(dc, oldBmp);
    DeleteDC(dc);

    HDC tmp = CreateCompatibleDC(screen);
    DWORD* tp = nullptr;
    HBITMAP tbmp = CreateDIBSection(tmp, (BITMAPINFO*)&bih, DIB_RGB_COLORS, (void**)&tp, nullptr, 0);
    HGDIOBJ oldTbmp = SelectObject(tmp, tbmp);

    int gs = cx * 9 / 16;
    if (gs < 7) gs = 7;
    HFONT font = CreateFontW(-gs, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe MDL2 Assets");
    HGDIOBJ oldFont = SelectObject(tmp, font);
    SetBkMode(tmp, TRANSPARENT);
    int ox = cx - gs, oy = cy - gs;
    wchar_t glyph[] = { 0xE713, 0 };

    std::memset(tp, 0, n * 4);
    SetTextColor(tmp, RGB(255, 255, 255));
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            TextOutW(tmp, ox + dx, oy + dy, glyph, 1);
    GdiFlush();
    for (int i = 0; i < n; i++)
        if (tp[i] & 0x00FFFFFF) px[i] = 0xFFFFFFFF;

    std::memset(tp, 0, n * 4);
    SetTextColor(tmp, RGB(80, 80, 80));
    TextOutW(tmp, ox, oy, glyph, 1);
    GdiFlush();
    for (int i = 0; i < n; i++)
        if (tp[i] & 0x00FFFFFF) px[i] = 0xFF000000 | (tp[i] & 0x00FFFFFF);

    SelectObject(tmp, oldFont);
    DeleteObject(font);
    SelectObject(tmp, oldTbmp);
    DeleteObject(tbmp);
    DeleteDC(tmp);

    HBITMAP mask = CreateBitmap(cx, cy, 1, 1, nullptr);
    HDC mdc = CreateCompatibleDC(screen);
    HGDIOBJ oldM = SelectObject(mdc, mask);
    PatBlt(mdc, 0, 0, cx, cy, BLACKNESS);
    SelectObject(mdc, oldM);
    DeleteDC(mdc);
    ReleaseDC(nullptr, screen);

    ICONINFO ii{ TRUE, 0, 0, mask, bmp };
    HICON result = CreateIconIndirect(&ii);
    DeleteObject(bmp);
    DeleteObject(mask);
    return result ? result : base;
}

static DWORD WINAPI tray_thread(LPVOID) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndproc; wc.hInstance = hInst;
    wc.lpszClassName = L"UUEnhanceTrayWnd";
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!g_wnd) { uu_log("tray window create failed"); return 0; }

    HICON ico = nullptr;
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(GetModuleHandleW(nullptr), exePath, MAX_PATH))
        ico = ExtractIconW(hInst, exePath, 0);
    if (!ico || ico == (HICON)1) ico = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    ico = overlayGear(ico);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_wnd; g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = ico;
    wcscpy_s(g_nid.szTip, L"UU远程增强");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    g_nid.uFlags = NIF_INFO;
    wcscpy_s(g_nid.szInfoTitle, L"UU远程增强 v" UURE_VERSION_W);
    wcscpy_s(g_nid.szInfo, L"已加载。被控期间仍可远控其他主机。");
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    uu_log("tray icon added");

    update::start(g_wnd, WM_UPDATE_FOUND);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    return 0;
}
void start_tray() { CreateThread(nullptr, 0, tray_thread, nullptr, 0, nullptr); }
