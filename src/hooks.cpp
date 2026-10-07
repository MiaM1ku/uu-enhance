#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>
#include <intrin.h>
#include "MinHook.h"
#include "offsets.h"
#include "config.h"
#include "log.h"
#include "resolver.h"
#include "hookset.h"
#include "session.h"

// 4.40：只绕过「本机已被控时禁止再当主控」。
// isControlled() 本身仍返回真值，被控页收起/展开不受影响。

static uintptr_t g_gvBase = 0;

// 诊断日志：写 %TEMP%\uu-enhance-diag.log（Win32 CreateFileW，不碰 CRT 文件锁），同时打 OutputDebugString。
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

// HomePageContent 次对象上的 isControlled()：读 this+0xFA 的一字节。
// 虚表槽 0xE0，实现经 thunk 跳到这里。
using fn_is_ctrl436_t = bool(__fastcall*)(void*);
static fn_is_ctrl436_t o_isCtrlNarrow436 = nullptr;
static uintptr_t g_isCtrlConnectRet436 = 0;
static uintptr_t g_isCtrlGuardRet436 = 0;
static bool __fastcall h_isCtrlNarrow436(void* thiz) {
    const uintptr_t ret = (uintptr_t)_ReturnAddress();
    if (ret == g_isCtrlConnectRet436 || ret == g_isCtrlGuardRet436) return false;
    return o_isCtrlNarrow436(thiz);
}

// DeviceDesktopScene::render：同步渲染期间改成「允许且未被控」。
// +0x69=0 会画「该设备不允许被控」；+0x6a=1 会藏进入桌面按钮。
// 4.42.1 底部工具栏（观看模式/文件传输）走 +0xD8 工具数组，不看 +0x8D。
// 数组每项 8 字节：DWORD toolId，+4 按钮启用，+5 是否上底栏。
// 只改 +4：改 +5 会改掉出栏集合，DesktopBottomWidget 的集合 diff 就整套重建按钮，
// 7 个 137px 的按钮加一个 52px 的「更多」塞不进 680px 宽的底栏，会挤成一排。
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
                if (g_deviceBottomToolsVecOff436) {
                    unsigned char* begin = *(unsigned char**)(data + g_deviceBottomToolsVecOff436);
                    unsigned char* end = *(unsigned char**)(data + g_deviceBottomToolsVecOff436 + 8);
                    if (begin && end && end >= begin && (size_t)(end - begin) <= 64) {
                        for (unsigned char* p = begin; p + 8 <= end; p += 8) overridePtr(p + 4, 1);
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

// DesktopButton 的启用状态（setEnabled 语义：a2=0 置 state 3=禁用）。
// 被控时数据层走这里把底栏那几个工具设成禁用，而 DesktopButton::event 里
// state==3 会直接跳过鼠标事件，表现就是点了没反应。钉成恒为启用。
using fn_button_state436_t = void(__fastcall*)(void*, char);
static fn_button_state436_t o_buttonState436 = nullptr;
static void __fastcall h_buttonState436(void* button, char enabled) {
    (void)enabled;
    if (o_buttonState436) o_buttonState436(button, 1);
}

// 工具启用判定 DeviceDetailPresenter::isToolEnabled(presenter, toolId, hasSession)。
// 被「已有会话」压掉：本机被控时 presenter+0x18 有入站会话，第三个参数为 1，
// 底栏四个工具（观看/文件传输/端口/终端，toolId 0-3）就全部判为不可用。
// onDeviceTool 每次点击都用它现算「已发布工具」列表，+4 为 0 直接 not_published 拒绝。
// 转发时把会话参数强制当 0（当作没有会话）：设备能力判定（sub_140A85BA0）和
// 云设备分支（toolId=4 的平台/能力检查）都原样保留。
using fn_tool_enabled436_t = unsigned char(__fastcall*)(void*, unsigned int, unsigned char);
static fn_tool_enabled436_t o_toolEnabled436 = nullptr;
static unsigned char __fastcall h_toolEnabled436(void* presenter, unsigned int toolId, unsigned char hasSession) {
    (void)hasSession;
    return o_toolEnabled436 ? o_toolEnabled436(presenter, toolId, 0) : 0;
}

// ===== 观看模式工具栏诊断：工具栏构造时 +112=0、+120 低字节=0，默认隐藏，
// 只有「布局初始化」把 32 字节观看初始状态经 state_applied 写进去才显示。
// 链条：观看状态槽(0x5D3D90，要求 this+0x10 内容对象非空)
//    → 布局初始化(0x59C300，先 ensureToolbar 再按 weak 块取工具栏)
//    → state_applied(0x624FB0，把状态拷到工具栏 +0x70)
//    → 可见性判定(0x625A50，+106/+112/+120 全非零才显示)。
// 每环各埋一个探针，一次运行定位断点。日志行：[vt-slot] [vt-init] [vt-apply] [vt-vis]。
using fn_vt_slot436_t = void(__fastcall*)(void*, void*);
static fn_vt_slot436_t o_vtSlot436 = nullptr;
static void __fastcall h_vtSlot436(void* self, void* state) {
    __try {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "[vt-slot] self=%p content=%p state=%p t=%lu", self,
                      self ? *(void**)((unsigned char*)self + 16) : nullptr, state,
                      (unsigned long)GetTickCount());
        diag_write(buf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (o_vtSlot436) o_vtSlot436(self, state);
}

using fn_vt_init436_t = void(__fastcall*)(void*, void*);
static fn_vt_init436_t o_vtInit436 = nullptr;
static void __fastcall h_vtInit436(void* content, void* state) {
    __try {
        char buf[192];
        unsigned char* s = (unsigned char*)state;
        unsigned int head = 0;
        if (s) head = *(unsigned int*)(s + 28);
        std::snprintf(buf, sizeof(buf), "[vt-init] content=%p state=%p s0=%u s8=%u s9=%u s1b=%u weakblk=%p t=%lu",
                      content, state, s ? (unsigned)s[0] : 999u, s ? (unsigned)s[8] : 999u,
                      s ? (unsigned)s[9] : 999u, head,
                      content ? *(void**)((unsigned char*)content + 56) : nullptr,
                      (unsigned long)GetTickCount());
        diag_write(buf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (o_vtInit436) o_vtInit436(content, state);
}

using fn_vt_apply436_t = void(__fastcall*)(void*, void*);
static fn_vt_apply436_t o_vtApply436 = nullptr;
static void __fastcall h_vtApply436(void* toolbar, void* state) {
    __try {
        char buf[160];
        unsigned char* s = (unsigned char*)state;
        std::snprintf(buf, sizeof(buf), "[vt-apply] toolbar=%p s0=%u s8=%u s9=%u t=%lu", toolbar,
                      s ? (unsigned)s[0] : 999u, s ? (unsigned)s[8] : 999u, s ? (unsigned)s[9] : 999u,
                      (unsigned long)GetTickCount());
        diag_write(buf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (o_vtApply436) o_vtApply436(toolbar, state);
}

using fn_vt_vis436_t = void(__fastcall*)(void*);
static fn_vt_vis436_t o_vtVis436 = nullptr;
static void __fastcall h_vtVis436(void* toolbar) {
    static int n = 0;
    if (n < 6) {
        ++n;
        __try {
            unsigned char* t = (unsigned char*)toolbar;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "[vt-vis] #%d toolbar=%p f106=%u f112=%u f120=%u t=%lu", n, toolbar,
                          (unsigned)t[106], (unsigned)t[112], (unsigned)t[120],
                          (unsigned long)GetTickCount());
            diag_write(buf);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (o_vtVis436) o_vtVis436(toolbar);
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
    if (V.deviceBottomButtonStateRva)
        hookset::install_at((void*)(base + V.deviceBottomButtonStateRva), "deviceBottomButtonState", "rva",
                            (void*)h_buttonState436, (void**)&o_buttonState436, rec);
    if (V.deviceToolEnabledCheckRva)
        hookset::install_at((void*)(base + V.deviceToolEnabledCheckRva), "deviceToolEnabledCheck", "rva",
                            (void*)h_toolEnabled436, (void**)&o_toolEnabled436, rec);
    if (V.vtSlotRva)
        hookset::install_at((void*)(base + V.vtSlotRva), "vtSlot", "rva",
                            (void*)h_vtSlot436, (void**)&o_vtSlot436, rec);
    if (V.vtInitRva)
        hookset::install_at((void*)(base + V.vtInitRva), "vtInit", "rva",
                            (void*)h_vtInit436, (void**)&o_vtInit436, rec);
    if (V.vtApplyRva)
        hookset::install_at((void*)(base + V.vtApplyRva), "vtApply", "rva",
                            (void*)h_vtApply436, (void**)&o_vtApply436, rec);
    if (V.vtVisRva)
        hookset::install_at((void*)(base + V.vtVisRva), "vtVis", "rva",
                            (void*)h_vtVis436, (void**)&o_vtVis436, rec);
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
