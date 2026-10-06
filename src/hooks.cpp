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

// HomePageContent 的 this：isControlled 是从次对象（this+0x30）调进来的，
// 退回去就是主页内容本体。先记下来备用（绕过 presenter 直接发起远控要用）。
static void* g_homePageThis = nullptr;
static void remember_home_page(void* thiz) {
    if (g_homePageThis || !thiz) return;
    g_homePageThis = (unsigned char*)thiz - 0x30;
    void* vtbl = nullptr;
    __try {
        vtbl = *(void**)g_homePageThis;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        vtbl = nullptr;
    }
    char buf[200];
    std::snprintf(buf, sizeof(buf), "[homepage-this] thiz=%p this=%p vtbl=%p vtblRva=%llx t=%lu", thiz,
                  g_homePageThis, vtbl, (unsigned long long)((uintptr_t)vtbl - g_gvBase),
                  (unsigned long)GetTickCount());
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

// DesktopButton 的启用状态：数据层在被控时把底栏那几项设成禁用，
// 而 DesktopButton::event 里 state==3 会直接跳过鼠标事件，所以点了没反应。
// 直接把这条路径钉成「启用」。
using fn_button_state436_t = void(__fastcall*)(void*, char);
static fn_button_state436_t o_buttonState436 = nullptr;

// 谁在动按钮状态（前 8 次都记，含 enabled）：确认禁用这条路到底走没走。
static void diag_btnstate(char enabled, uintptr_t ret) {
    static int n = 0;
    if (n >= 8) return;
    ++n;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "[btn-state] #%d en=%d rva=%llx t=%lu", n, (int)enabled,
                  (unsigned long long)(ret - g_gvBase), (unsigned long)GetTickCount());
    diag_write(buf);
}

static void __fastcall h_buttonState436(void* button, char enabled) {
    diag_btnstate(enabled, (uintptr_t)_ReturnAddress());
    if (o_buttonState436) o_buttonState436(button, 1);
}

// HomePageContent::startRemoteAssist()（点击工具后的命令入口）。
// 它开头检查 this+300（设备数据是否已初始化），不满足就静默 return。
using fn_start_ra436_t = void(__fastcall*)(void*);
static fn_start_ra436_t o_startRA436 = nullptr;

// 点击动作 + 观看模式的直连。
// presenter 那条路要求 home_frame_model 活着，而它在被控状态下已经销毁
// （weak_ptr 过期，_Ptr 虽然还留着但对象没了），所以观看模式直接走主页的发起入口，
// 绕开 DeviceDetailPresenter 的 model 依赖。
using fn_button_click436_t = void(__fastcall*)(void*);
static fn_button_click436_t o_buttonClick436 = nullptr;
static void __fastcall h_buttonClick436(void* button) {
    static int n = 0;
    if (n < 10) {
        ++n;
        __try {
            char buf[200];
            std::snprintf(buf, sizeof(buf), "[btn-click] #%d tool=%u flag72=%u id64=%u t=%lu", n,
                          *(unsigned int*)((unsigned char*)button + 68),
                          (unsigned)*(unsigned char*)((unsigned char*)button + 72),
                          *(unsigned int*)((unsigned char*)button + 64),
                          (unsigned long)GetTickCount());
            diag_write(buf);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (o_buttonClick436) o_buttonClick436(button);

    __try {
        const unsigned int tool = *(unsigned int*)((unsigned char*)button + 68);
        if (tool == 0 && g_homePageThis && o_startRA436) {
            char buf[200];
            std::snprintf(buf, sizeof(buf), "[direct-start] flag300=%u this=%p t=%lu",
                          (unsigned)*(unsigned char*)((unsigned char*)g_homePageThis + 300), g_homePageThis,
                          (unsigned long)GetTickCount());
            diag_write(buf);
            o_startRA436(g_homePageThis);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// 信号链探针（只做诊断）：点击工具后信号逐跳转发，看链在哪一跳断掉。
// L1 按钮点击信号 → L2 底栏 deviceToolRequested → L3 底栏容器信号0 → L4 场景信号0。
using fn_sig_probe436_t = void(__fastcall*)(void*, int);
static fn_sig_probe436_t o_sigProbe436[4] = { nullptr, nullptr, nullptr, nullptr };

static void diag_sigprobe(int idx, int arg) {
    static int cnt[4] = { 0, 0, 0, 0 };
    if (idx < 0 || idx > 3 || cnt[idx] >= 8) return;
    ++cnt[idx];
    char buf[128];
    std::snprintf(buf, sizeof(buf), "[sig-L%d] #%d arg=%d t=%lu", idx + 1, cnt[idx], arg,
                  (unsigned long)GetTickCount());
    diag_write(buf);
}

static void __fastcall h_sigProbe0(void* o, int v) { diag_sigprobe(0, v); if (o_sigProbe436[0]) o_sigProbe436[0](o, v); }
static void __fastcall h_sigProbe1(void* o, int v) { diag_sigprobe(1, v); if (o_sigProbe436[1]) o_sigProbe436[1](o, v); }
static void __fastcall h_sigProbe2(void* o, int v) { diag_sigprobe(2, v); if (o_sigProbe436[2]) o_sigProbe436[2](o, v); }
static void __fastcall h_sigProbe3(void* o, int v) { diag_sigprobe(3, v); if (o_sigProbe436[3]) o_sigProbe436[3](o, v); }

// 工具请求的接收槽：DeviceDesktopView::onDeviceTool(view, toolId)。
// 它读 view+0x60 的 presenter，为空就静默拒绝（日志 presenter_unavailable）。
using fn_tool_slot436_t = long long(__fastcall*)(void*, unsigned int);
static fn_tool_slot436_t o_toolSlot436 = nullptr;
static long long __fastcall h_toolSlot436(void* view, unsigned int tool) {
    static int n = 0;
    if (n < 12) {
        ++n;
        __try {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "[tool-slot] #%d tool=%u view=%p presenter=%p t=%lu", n, tool, view,
                          view ? *(void**)((unsigned char*)view + 96) : nullptr, (unsigned long)GetTickCount());
            diag_write(buf);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return o_toolSlot436 ? o_toolSlot436(view, tool) : 0;
}

// presenter 注入点：能看到 presenter 什么时候被设置、什么时候被清空。
using fn_set_presenter436_t = void(__fastcall*)(void*, void*, void*);
static fn_set_presenter436_t o_setPresenter436 = nullptr;
static void __fastcall h_setPresenter436(void* view, void* a2, void* a3) {
    static int n = 0;
    if (n < 12) {
        ++n;
        __try {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "[set-presenter] #%d view=%p new=%p t=%lu", n, view,
                          a3 ? *(void**)a3 : nullptr, (unsigned long)GetTickCount());
            diag_write(buf);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (o_setPresenter436) o_setPresenter436(view, a2, a3);
}

// presenter 的工具处理（只做诊断）：看它收到的 toolId，以及观看模式依赖的
// home_frame_model（presenter+0x40，weak_ptr）到底还在不在。
using fn_presenter_tool436_t = long long(__fastcall*)(void*, void*);
static fn_presenter_tool436_t o_presenterTool436 = nullptr;
static long long __fastcall h_presenterTool436(void* presenter, void* arg) {
    static int n = 0;
    if (n < 6) {
        ++n;
        __try {
            char buf[200];
            std::snprintf(buf, sizeof(buf), "[presenter-tool] #%d tool=%u model=%p flag320=%u t=%lu", n,
                          *(unsigned int*)((unsigned char*)arg + 32),
                          *(void**)((unsigned char*)presenter + 40),
                          (unsigned)*(unsigned char*)((unsigned char*)presenter + 320),
                          (unsigned long)GetTickCount());
            diag_write(buf);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return o_presenterTool436 ? o_presenterTool436(presenter, arg) : 0;
}

// startRemoteAssist 的调用记录（限流）。
static void __fastcall h_startRA436(void* self) {
    static int n = 0;
    if (n < 12) {
        ++n;
        __try {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "[start-ra] #%d flag300=%d t=%lu", n,
                          (int)*((unsigned char*)self + 300), (unsigned long)GetTickCount());
            diag_write(buf);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (o_startRA436) o_startRA436(self);
}

// 命令分发器（只做诊断）：记录收到了哪些命令名。
using fn_cmd_dispatch436_t = void(__fastcall*)(void*, const char*, void*);
static fn_cmd_dispatch436_t o_cmdDispatch436 = nullptr;
static void __fastcall h_cmdDispatch436(void* self, const char* name, void* cb) {
    static int n = 0;
    if (n < 12) {
        ++n;
        __try {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "[cmd] #%d name=%s t=%lu", n, name ? name : "?",
                          (unsigned long)GetTickCount());
            diag_write(buf);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (o_cmdDispatch436) o_cmdDispatch436(self, name, cb);
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
    remember_home_page(thiz);
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
        char buf[160];
        std::snprintf(buf, sizeof(buf), "[hook] %s how=%s ok=%d rva=%llx", name, how ? how : "",
                      ok ? 1 : 0, (unsigned long long)(addr ? (uintptr_t)addr - g_gvBase : 0));
        diag_write(buf);
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
    if (V.deviceBottomButtonClickRva)
        hookset::install_at((void*)(base + V.deviceBottomButtonClickRva), "deviceBottomButtonClick", "rva",
                            (void*)h_buttonClick436, (void**)&o_buttonClick436, rec);
    if (V.homePageStartRemoteAssistRva)
        hookset::install_at((void*)(base + V.homePageStartRemoteAssistRva), "homePageStartRemoteAssist", "rva",
                            (void*)h_startRA436, (void**)&o_startRA436, rec);
    if (V.cmdDispatchRva)
        hookset::install_at((void*)(base + V.cmdDispatchRva), "commandDispatch", "rva",
                            (void*)h_cmdDispatch436, (void**)&o_cmdDispatch436, rec);
    {
        void* const kProbeFn[4] = { (void*)h_sigProbe0, (void*)h_sigProbe1, (void*)h_sigProbe2, (void*)h_sigProbe3 };
        const char* const kProbeName[4] = { "sigProbeL1", "sigProbeL2", "sigProbeL3", "sigProbeL4" };
        for (int i = 0; i < 4; ++i) {
            if (!V.sigProbeRva[i]) continue;
            hookset::install_at((void*)(base + V.sigProbeRva[i]), kProbeName[i], "rva",
                                kProbeFn[i], (void**)&o_sigProbe436[i], rec);
        }
    }
    if (V.deviceToolSlotRva)
        hookset::install_at((void*)(base + V.deviceToolSlotRva), "deviceToolSlot", "rva",
                            (void*)h_toolSlot436, (void**)&o_toolSlot436, rec);
    if (V.setPresenterRva)
        hookset::install_at((void*)(base + V.setPresenterRva), "setPresenter", "rva",
                            (void*)h_setPresenter436, (void**)&o_setPresenter436, rec);
    if (V.presenterToolRva)
        hookset::install_at((void*)(base + V.presenterToolRva), "presenterTool", "rva",
                            (void*)h_presenterTool436, (void**)&o_presenterTool436, rec);
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
