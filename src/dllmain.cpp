#include <windows.h>
#include "app.h"
#include "config.h"
#include "log.h"

void proxy_init();
void install_hooks(uintptr_t base);

static bool exe_basename_is(const wchar_t* name) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
    const wchar_t* b = wcsrchr(path, L'\\');
    return lstrcmpiW(b ? b + 1 : path, name) == 0;
}

static DWORD WINAPI init_thread(LPVOID) {
    wchar_t exePath[MAX_PATH]{}; GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (!exe_basename_is(L"GameViewer.exe")) {
        uu_log("v%s skip non-controller process: %ls", UURE_VERSION, exePath);
        return 0;
    }
    uintptr_t base = (uintptr_t)GetModuleHandleW(L"GameViewer.exe");
    uu_log("v%s controller GameViewer.exe base=%p", UURE_VERSION, (void*)base);
    cfg::load();
    install_hooks(base);
    uu_log("controller ready");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        proxy_init();
        CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
    }
    return TRUE;
}
