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
