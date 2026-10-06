#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>
#include <cstdio>
#include <intrin.h>
#include "MinHook.h"
#include "offsets.h"
#include "config.h"
#include "log.h"
#include "resolver.h"
#include "hookset.h"
#include "session.h"

static uintptr_t g_gvBase = 0;   // GameViewer 模块基址，install_hooks 里赋值

// 实机诊断：同时写 %TEMP%\uu-enhance-diag.log 和 OutputDebugString。
// 只用 kernel32 的 CreateFileW/WriteFile，不碰 CRT 的文件锁，
// 因为它会在渲染线程里被调用，CRT 的 fopen 在那种上下文出过事。
static void diag_write(const char* line) {
    __try {
        wchar_t dir[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH, dir)) {
            wchar_t path[MAX_PATH]{};
            size_t n = 0;
            while (dir[n] && n < MAX_PATH - 32) { path[n] = dir[n]; ++n; }
            const wchar_t* tail = L"uu-enhance-diag.log";
            for (size_t i = 0; tail[i] && n + i < MAX_PATH - 1; ++i) path[n + i] = tail[i];
            path[MAX_PATH - 1] = 0;
            HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(h, line, (DWORD)std::strlen(line), &written, nullptr);
                WriteFile(h, "\r\n", 2, &written, nullptr);
                CloseHandle(h);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    uu_log("%s", line);
}

// isControlled() 的调用者（去重，最多 24 条）：用来定位「工具能不能用」的判定在哪。
static void diag_isctrl(uintptr_t ret) {
    static uintptr_t seen[24]{};
    static int n = 0;
    for (int i = 0; i < n; ++i) if (seen[i] == ret) return;
    if (n >= 24) return;
    seen[n++] = ret;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "[isctrl] #%d ret=%p rva=%llx t=%lu", n, (void*)ret,
                  (unsigned long long)(ret - g_gvBase), (unsigned long)GetTickCount());
    diag_write(buf);
}

// 底栏工具数组的原始内容（前 3 次）：确认官方给 +4/+5 的到底是什么。
static void diag_tools(unsigned char* data, unsigned char* begin, unsigned char* end) {
    static int done = 0;
    if (done >= 3) return;
    ++done;
    char buf[640];
    int n = std::snprintf(buf, sizeof(buf), "[tools] #%d data=%p count=%u t=%lu", done, (void*)data,
                          (unsigned)((end - begin) / 8), (unsigned long)GetTickCount());
    for (unsigned char* p = begin; p + 8 <= end && n > 0 && n < (int)sizeof(buf) - 48; p += 8) {
        n += std::snprintf(buf + n, sizeof(buf) - n, " id=%u en=%u vis=%u",
                           *(unsigned int*)p, (unsigned)p[4], (unsigned)p[5]);
    }
    diag_write(buf);
}

// 4.40：只绕过「本机已被控时禁止再当主控」。
// isControlled() 本身仍返回真值，被控页收起/展开不受影响。

using fn_is_ctrl436_t = bool(__fastcall*)(void*);
static fn_is_ctrl436_t o_isCtrlNarrow436 = nullptr;
static uintptr_t g_isCtrlConnectRet436 = 0;
static uintptr_t g_isCtrlGuardRet436 = 0;
static bool __fastcall h_isCtrlNarrow436(void* thiz) {
    const uintptr_t ret = (uintptr_t)_ReturnAddress();
    diag_isctrl(ret);
    if (ret == g_isCtrlConnectRet436 || ret == g_isCtrlGuardRet436) return false;
    return o_isCtrlNarrow436(thiz);
}

// DeviceDesktopScene::render：同步渲染期间改成「允许且未被控」。
// +0x69=0 会画「该设备不允许被控」；+0x6a=1 会藏进入桌面按钮。
// 4.42.1 底部工具栏（观看模式/文件传输）走 +0xD8 工具数组，不看 +0x8D。
// 数组每项 8 字节：DWORD toolId，+4 按钮启用，+5 是否上底栏。
// 只改 +4。改 +5 会让 DesktopBottomWidget 把官方收在「更多工具」里的项
// 一起拉到 680px 宽的底栏上，8 个图标挤成一排。
using fn_device_scene_render436_t = void(__fastcall*)(void*, unsigned char*);
static fn_device_scene_render436_t o_deviceSceneRender436 = nullptr;
static uintptr_t g_deviceDesktopAllowedOff436 = 0;
static uintptr_t g_deviceDesktopControlledOff436 = 0;
static uintptr_t g_deviceActionAllowedOff436 = 0;
static uintptr_t g_deviceActionControlledOff436 = 0;
static uintptr_t g_deviceActionEnabledOff436 = 0;
static uintptr_t g_deviceBottomToolsVecOff436 = 0;
static void __fastcall h_deviceSceneRender436(void* scene, unsigned char* data) {
    struct SavedFlag {
        unsigned char* ptr;
        unsigned char value;
    } saved[24]{};
    size_t savedCount = 0;
    __try {
        if (data && g_deviceDesktopAllowedOff436 && g_deviceDesktopControlledOff436) {
            const unsigned int platform = *(unsigned int*)(data + 0x60);
            if (platform == 1 || platform == 4) {
                auto overridePtr = [&](unsigned char* ptr, unsigned char value) {
                    if (!ptr || savedCount >= sizeof(saved) / sizeof(saved[0])) return;
                    saved[savedCount++] = { ptr, *ptr };
                    *ptr = value;
                };
                auto overrideFlag = [&](uintptr_t off, unsigned char value) {
                    if (!off) return;
                    overridePtr(data + off, value);
                };
                overrideFlag(g_deviceDesktopAllowedOff436, 1);
                overrideFlag(g_deviceDesktopControlledOff436, 0);
                overrideFlag(g_deviceActionAllowedOff436, 1);
                overrideFlag(g_deviceActionControlledOff436, 0);
                overrideFlag(g_deviceActionEnabledOff436, 1);
                // 底栏工具项：只置启用位，保持官方的出栏集合（+5 不动，否则整套重建、挤一排）。
                // 这一位必须回滚：试过常驻置 1（v1.4.3），启动就崩，原因没坐实，
                // 改成回滚是已知能跑的做法。
                if (g_deviceBottomToolsVecOff436) {
                    unsigned char* begin = *(unsigned char**)(data + g_deviceBottomToolsVecOff436);
                    unsigned char* end = *(unsigned char**)(data + g_deviceBottomToolsVecOff436 + 8);
                    if (begin && end && end >= begin && (size_t)(end - begin) <= 64) {
                        diag_tools(data, begin, end);
                        for (unsigned char* p = begin; p + 8 <= end; p += 8) {
                            overridePtr(p + 4, 1);
                        }
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        savedCount = 0;
    }

    o_deviceSceneRender436(scene, data);

    __try {
        while (savedCount) {
            --savedCount;
            *saved[savedCount].ptr = saved[savedCount].value;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static std::mutex g_dbgMtx;
static std::vector<HookStat> g_hookStats;
static std::wstring g_gvVersion;
static bool g_verKnown = false;

struct InProcRecorder : hookset::IRecorder {
    void record(const char* name, void* addr, const char* how, bool ok) override {
        std::lock_guard<std::mutex> lk(g_dbgMtx);
        g_hookStats.push_back({ name, addr, how, ok });
    }
};

void install_hooks(uintptr_t base) {
    if (MH_Initialize() != MH_OK) { uu_log("MH_Initialize failed"); return; }
    std::wstring vs = cfg::exe_version();
    const ver::VerSet& V = ver::pick(vs.c_str());
    g_verKnown = (!vs.empty() && vs == V.version);
    g_gvBase = base;
    g_gvVersion = vs.empty() ? L"?" : vs;
    uu_log("GameViewer version=%ls known=%d", vs.empty() ? L"?" : vs.c_str(), (int)g_verKnown);
    if (!g_verKnown) {
        uu_log("GameViewer: unsupported version, skip hooks");
        return;
    }
    resolver::ModRange r{};
    resolver::get_ranges((HMODULE)base, r);
    if (r.img_end - r.img_beg != V.imageSize
        || !resolver::find_string(r, "startRemoteAssist: device data is not init, return")
        || !resolver::find_string(r, "control_mode_switch")) {
        uu_log("GameViewer: layout guard mismatch, refusing RVA hooks");
        return;
    }
    InProcRecorder rec;
    g_isCtrlConnectRet436 = base + V.isCtrlConnectRetRva;
    g_isCtrlGuardRet436 = base + V.isCtrlGuardRetRva;
    hookset::install_at((void*)(base + V.isCtrlNarrowRva), "isControlledConnectOnly", "rva",
                        (void*)h_isCtrlNarrow436, (void**)&o_isCtrlNarrow436, rec);
    g_deviceDesktopAllowedOff436 = V.deviceDesktopAllowedOff;
    g_deviceDesktopControlledOff436 = V.deviceDesktopControlledOff;
    g_deviceActionAllowedOff436 = V.deviceActionAllowedOff;
    g_deviceActionControlledOff436 = V.deviceActionControlledOff;
    g_deviceActionEnabledOff436 = V.deviceActionEnabledOff;
    g_deviceBottomToolsVecOff436 = V.deviceBottomToolsVecOff;
    hookset::install_at((void*)(base + V.deviceSceneRenderRva), "deviceDesktopControlAllowed", "rva",
                        (void*)h_deviceSceneRender436, (void**)&o_deviceSceneRender436, rec);
    uu_log("install_hooks done");
}

DebugInfo debug_snapshot() {
    std::lock_guard<std::mutex> lk(g_dbgMtx);
    DebugInfo d;
    d.gvVersion = g_gvVersion;
    d.gvKnown = g_verKnown;
    for (const auto& h : g_hookStats)
        d.hooks.push_back({ L"ctl", h.name, h.how, h.addr ? (unsigned long long)((uintptr_t)h.addr - g_gvBase) : 0, h.ok });
    return d;
}
