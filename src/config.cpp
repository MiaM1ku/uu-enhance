#include "config.h"
#include <windows.h>
#include <shlobj.h>
#include <string>

namespace cfg {
    std::atomic<bool> g_autoUpdate{true};

    static std::wstring iniDir() {
        wchar_t buf[MAX_PATH]{};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, buf)))
            return std::wstring(buf) + L"\\uu-enhance";
        return L"";
    }
    static std::wstring iniPath() {
        std::wstring d = iniDir();
        return d.empty() ? L"" : d + L"\\uu-enhance.ini";
    }

    void load() {
        auto p = iniPath();
        g_autoUpdate = GetPrivateProfileIntW(L"general", L"auto_update_check", 1, p.c_str()) != 0;
    }

    void save() {
        std::wstring d = iniDir();
        if (!d.empty()) CreateDirectoryW(d.c_str(), nullptr);
        auto p = iniPath();
        if (p.empty()) return;
        WritePrivateProfileStringW(L"general", L"auto_update_check", g_autoUpdate ? L"1" : L"0", p.c_str());
    }

    std::wstring config_path() { return iniPath(); }

    std::wstring exe_version() {
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return L"";
        wchar_t sys[MAX_PATH]{}; GetSystemDirectoryW(sys, MAX_PATH); wcscat_s(sys, L"\\version.dll");
        HMODULE v = LoadLibraryW(sys);
        if (!v) return L"";
        auto pSize = (DWORD(WINAPI*)(LPCWSTR, LPDWORD))GetProcAddress(v, "GetFileVersionInfoSizeW");
        auto pGet  = (BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID))GetProcAddress(v, "GetFileVersionInfoW");
        auto pQry  = (BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT))GetProcAddress(v, "VerQueryValueW");
        std::wstring out;
        if (pSize && pGet && pQry) {
            DWORD h = 0, sz = pSize(path, &h);
            if (sz) {
                std::wstring buf(sz, 0);
                VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
                if (pGet(path, 0, sz, &buf[0]) && pQry(&buf[0], L"\\", (LPVOID*)&fi, &len) && fi) {
                    wchar_t s[64];
                    swprintf_s(s, L"%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                               HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
                    out = s;
                }
            }
        }
        FreeLibrary(v);
        return out;
    }
}
