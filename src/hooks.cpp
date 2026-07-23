#include <windows.h>
#include <set>
#include <map>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <iterator>
#include <cstdint>
#include "MinHook.h"
#include "offsets.h"
#include "config.h"
#include "log.h"
#include "resolver.h"
#include "session.h"

// 原函数指针（trampoline）
using fn_send_t   = void*(__fastcall*)(void*, void*, void*, void*, void*, void*, void*, void*);
using fn_cap_t    = void (__fastcall*)(void* thiz, unsigned __int8 enable, char toast, char a4);
using fn_clipupd_t= void (__fastcall*)(void* thiz);
using fn_fmtlist_t= __int64(__fastcall*)(void* thiz, void* a2, void* a3);
using fn_clipget_t= __int64(__fastcall*)(void* hwnd, unsigned int fmt, void* out);
using fn_sendfmt_t= __int64(__fastcall*)(void* thiz);
using fn_gpupd_t  = void (__fastcall*)(void* thiz, void* padState);

static fn_send_t    o_sendMouse = nullptr, o_sendWheel = nullptr, o_sendKey = nullptr;
static fn_cap_t     o_enableCapture = nullptr;
static fn_clipupd_t o_clipUpdate = nullptr;
static fn_fmtlist_t o_clipFmtList = nullptr;
static fn_clipget_t o_clipGet = nullptr;
static fn_sendfmt_t o_clipSendFmt = nullptr;
static fn_gpupd_t   o_gamepadUpdate = nullptr, o_gamepadConnect = nullptr, o_gamepadDisconnect = nullptr;

// CCS 内 device_id (std::string) 偏移，由所选版本表设置；仅用于去重/回退名(读错不影响分会话)
static uintptr_t CCS_DEVICE_ID_OFF = 3984;

// 每个会话的状态，用 CCS 指针做 key
struct SessState { bool viewOnly; bool clipSync; bool gamepadOff; std::wstring devid; std::wstring name; DWORD lastNameTick; };
static std::mutex                 g_smtx;
static std::map<void*, SessState> g_sessions;
static void*                      g_activeCCS = nullptr;   // 最近有输入事件的会话(前台)

// SEH 安全读取 CCS+OFF 处 std::string 的字节到 POD 缓冲(无 C++ 对象，可用 __try)
static int safe_copy_devid(void* ccs, char* buf, int bufsz) {
    __try {
        char* s = (char*)ccs + CCS_DEVICE_ID_OFF;
        size_t len = *(size_t*)(s + 16);
        size_t cap = *(size_t*)(s + 24);
        const char* p = (cap >= 16) ? *(const char**)s : s;
        if (!p || len == 0 || len >= (size_t)bufsz) return 0;   // 用 size_t 比较，len 是垃圾大值也不会变负绕过
        for (size_t i = 0; i < len; ++i) { unsigned char c = (unsigned char)p[i]; if (c < 0x20) return 0; buf[i] = p[i]; }
        return (int)len;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static std::wstring read_device_id(void* ccs) {
    char buf[129];
    int len = safe_copy_devid(ccs, buf, sizeof(buf));
    if (len <= 0) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, buf, len, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, buf, len, &w[0], n);
    return w;
}


// 取/建会话状态(持 g_smtx)
static SessState& sessOf(void* ccs) {
    auto it = g_sessions.find(ccs);
    if (it != g_sessions.end()) return it->second;
    SessState s;
    s.viewOnly   = cfg::g_viewOnly.load();   // 新会话默认值
    s.clipSync   = cfg::g_clipSync.load();
    s.gamepadOff = cfg::g_gamepadOff.load();
    s.devid       = read_device_id(ccs);       // 用于去重
    s.name        = s.devid;                    // 回退显示名，随后由窗口标题覆盖
    s.lastNameTick= 0;
    uu_log("session new: ccs=%p devid=%ls viewOnly=%d", ccs, s.devid.c_str(), (int)s.viewOnly);
    return g_sessions.emplace(ccs, std::move(s)).first->second;
}
// 输入 hook 用：记录活动会话，返回该会话是否仅浏览。
// 设备名取自前台窗口标题（UU 把视频窗口标题设成了设备名），每秒最多抓一次。
// 抓标题不能在持锁时做：同进程窗口的 GetWindowText 会同步发 WM_GETTEXT 回 UI 线程，
// 持锁期间跑宿主代码有重入死锁风险。所以锁内只标记活动会话，锁外读标题，再短暂回锁写回。
static bool input_viewOnly(void* ccs) {
    bool vo, wantName = false;
    {
        std::lock_guard<std::mutex> lk(g_smtx);
        g_activeCCS = ccs;
        SessState& s = sessOf(ccs);
        vo = s.viewOnly;
        DWORD now = GetTickCount();
        if (now - s.lastNameTick >= 1000) { s.lastNameTick = now; wantName = true; }
    }
    if (wantName) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        wchar_t t[128];
        int n = (fg && pid == GetCurrentProcessId()) ? GetWindowTextW(fg, t, 128) : 0;  // 只认本进程窗口
        if (n > 0) {
            std::lock_guard<std::mutex> lk(g_smtx);
            auto it = g_sessions.find(ccs);
            if (it != g_sessions.end()) it->second.name.assign(t, n);
        }
    }
    return vo;
}
static bool active_viewOnly() {
    std::lock_guard<std::mutex> lk(g_smtx);
    if (!g_activeCCS) return cfg::g_viewOnly.load();
    return sessOf(g_activeCCS).viewOnly;
}
static bool active_clipSync() {
    std::lock_guard<std::mutex> lk(g_smtx);
    if (!g_activeCCS) return cfg::g_clipSync.load();
    return sessOf(g_activeCCS).clipSync;
}
static bool active_gpBlock() {
    std::lock_guard<std::mutex> lk(g_smtx);
    if (!g_activeCCS) return cfg::g_viewOnly.load() || cfg::g_gamepadOff.load();
    auto& s = sessOf(g_activeCCS);
    return s.viewOnly || s.gamepadOff;
}

// 仅浏览：拦掉输入发送
static void* __fastcall h_sendMouse(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    if (input_viewOnly(a1)) return nullptr;
    return o_sendMouse(a1, a2, a3, a4, a5, a6, a7, a8);
}
static void* __fastcall h_sendWheel(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    if (input_viewOnly(a1)) return nullptr;
    return o_sendWheel(a1, a2, a3, a4, a5, a6, a7, a8);
}
static void* __fastcall h_sendKey(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    if (input_viewOnly(a1)) return nullptr;
    return o_sendKey(a1, a2, a3, a4, a5, a6, a7, a8);
}
// 仅浏览时别让它锁鼠标，把 enable 当成 0
static void __fastcall h_enableCapture(void* thiz, unsigned __int8 enable, char toast, char a4) {
    if (active_viewOnly()) enable = 0;
    o_enableCapture(thiz, enable, toast, a4);
}

// 剪贴板。出向 on_clipboard_update 挡掉就不外推本地剪贴板。
// 入向不能掐整个分发器(handle_clipboard_request)——那是请求/应答，掐了对端握手会卡死、
// 把没补丁的对端剪贴板搞坏。只钝化 do_handle_format_list_request 这个落地点：关同步时
// 不让它 EmptyClipboard+装延迟渲染桩，分发器照常把 FormatListResponse 应答给对端。
static void __fastcall h_clipUpdate(void* thiz) {
    if (!active_clipSync()) return;
    o_clipUpdate(thiz);
}
// 出向格式表通告。主控剪贴板一变就枚举本地格式发给对端，对端据此 EmptyClipboard+装延迟渲染桩。
// 这是主控主动发的(非应答)，关同步时直接不发，对端剪贴板就不会被清空。
static __int64 __fastcall h_clipSendFmt(void* thiz) {
    if (!active_clipSync()) return 0;
    return o_clipSendFmt(thiz);
}
static __int64 __fastcall h_clipFmtList(void* thiz, void* a2, void* a3) {
    if (!active_clipSync()) return 0;
    return o_clipFmtList(thiz, a2, a3);
}
// 出向数据服务点。对端粘贴时来拉主控剪贴板，最终经 get_clipboard_data 读本地剪贴板应答。
// 关同步时把输出 std::string 置空、返回失败——等同剪贴板为空(app 的正常分支)，对端拿到空、
// 不卡，且本地剪贴板没被读出去。out 是 MSVC std::string：[0..15]SSO/指针 [16]size [24]cap。
static __int64 __fastcall h_clipGet(void* hwnd, unsigned int fmt, void* out) {
    if (!active_clipSync()) {
        if (out) {
            size_t* s = (size_t*)out;
            char* buf = s[3] >= 0x10 ? *(char**)out : (char*)out;
            s[2] = 0;
            buf[0] = 0;
        }
        return 0;
    }
    return o_clipGet(hwnd, fmt, out);
}

// 手柄
static std::mutex        g_gpMtx;
static void*             g_gpMgr = nullptr;
static std::set<uint8_t> g_desired;          // 物理存在的手柄索引
// 切换仅浏览/禁手柄时，把已连手柄在被控端拔掉或重连。
// 先在锁内把实例和索引拷出来，解锁后再调原始函数——不在锁里跑宿主代码。
static void gp_reconcile(bool block) {
    void* mgr; std::vector<uint8_t> ids;
    {
        std::lock_guard<std::mutex> lk(g_gpMtx);
        mgr = g_gpMgr;
        ids.assign(g_desired.begin(), g_desired.end());
    }
    if (!mgr) return;
    for (uint8_t i : ids) {
        uint8_t v = i;
        if (block) { if (o_gamepadDisconnect) o_gamepadDisconnect(mgr, &v); }
        else       { if (o_gamepadConnect)    o_gamepadConnect(mgr, &v); }
    }
}
static void __fastcall h_gamepadConnect(void* thiz, void* idx) {
    { std::lock_guard<std::mutex> lk(g_gpMtx); g_gpMgr = thiz; if (idx) g_desired.insert(*(uint8_t*)idx); }
    if (active_gpBlock()) return;
    o_gamepadConnect(thiz, idx);
}
static void __fastcall h_gamepadDisconnect(void* thiz, void* idx) {
    { std::lock_guard<std::mutex> lk(g_gpMtx); g_gpMgr = thiz; if (idx) g_desired.erase(*(uint8_t*)idx); }
    o_gamepadDisconnect(thiz, idx);
}
static void __fastcall h_gamepadUpdate(void* thiz, void* padState) {
    { std::lock_guard<std::mutex> lk(g_gpMtx); g_gpMgr = thiz; }
    if (active_gpBlock()) return;
    o_gamepadUpdate(thiz, padState);
}

// 给托盘菜单用，声明在 session.h
std::vector<SessSnap> sessions_snapshot() {
    std::lock_guard<std::mutex> lk(g_smtx);
    std::vector<SessSnap> v;
    for (auto& kv : g_sessions) {
        const std::wstring& disp = !kv.second.name.empty() ? kv.second.name : kv.second.devid;
        v.push_back({ kv.first, disp, kv.second.viewOnly, kv.second.clipSync, kv.second.gamepadOff });
    }
    return v;
}
// field: 0=viewOnly 1=clipSync 2=gamepadOff ; 返回切换后的值
bool session_toggle(void* key, int field) {
    bool nv = false; bool doGpReconcile = false; bool block = false;
    {
        std::lock_guard<std::mutex> lk(g_smtx);
        auto it = g_sessions.find(key);
        if (it == g_sessions.end()) return false;
        auto& s = it->second;
        if (field == 0) { s.viewOnly = !s.viewOnly; nv = s.viewOnly; doGpReconcile = true; block = s.viewOnly || s.gamepadOff; }
        else if (field == 1) { s.clipSync = !s.clipSync; nv = s.clipSync; }
        else { s.gamepadOff = !s.gamepadOff; nv = s.gamepadOff; doGpReconcile = true; block = s.viewOnly || s.gamepadOff; }
    }
    if (field == 0 && nv) ClipCursor(nullptr);   // 立即释放鼠标
    if (doGpReconcile) gp_reconcile(block);
    uu_log("session_toggle key=%p field=%d -> %d", key, field, (int)nv);
    return nv;
}

// 会话注册/移除。UU 断开时不销毁 CCS（留着重连），所以不能 hook 析构，只能 hook 关闭和退出。
using fn4_t = __int64(__fastcall*)(void*, void*, void*, void*);
static fn4_t o_setConnInfo = nullptr, o_closeConn = nullptr, o_exitRoom = nullptr;

static void session_remove(void* ccs) {
    std::lock_guard<std::mutex> lk(g_smtx);
    if (g_sessions.erase(ccs)) uu_log("session remove: ccs=%p", ccs);
    if (g_activeCCS == ccs) g_activeCCS = nullptr;
}
static __int64 __fastcall h_setConnInfo(void* ccs, void* a2, void* a3, void* a4) {
    {
        std::lock_guard<std::mutex> lk(g_smtx);
        std::wstring devid = read_device_id(ccs);
        if (!devid.empty())  // 去重：移除同设备的旧(stale)会话
            for (auto it = g_sessions.begin(); it != g_sessions.end(); )
                it = (it->first != ccs && it->second.devid == devid) ? g_sessions.erase(it) : std::next(it);
        g_activeCCS = ccs;
        sessOf(ccs);   // 注册+置活动(名字待首次操作时由窗口标题抓取)
    }
    return o_setConnInfo(ccs, a2, a3, a4);
}
static __int64 __fastcall h_closeConn(void* ccs, void* a2, void* a3, void* a4) {
    session_remove(ccs);
    return o_closeConn(ccs, a2, a3, a4);
}
static __int64 __fastcall h_exitRoom(void* ccs, void* a2, void* a3, void* a4) {
    session_remove(ccs);
    return o_exitRoom(ccs, a2, a3, a4);
}

// 仅浏览时把光标显示成禁用图标，同时挡掉远端光标同步
using fn_curs_t = __int64(__fastcall*)(void*, unsigned int);
static fn_curs_t o_updateCursor = nullptr;
static __int64 __fastcall h_updateCursor(void* vw, unsigned int force) {
    if (active_viewOnly()) {
        SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_NO));  // 禁用标志，且不应用远端光标
        return 1;
    }
    return o_updateCursor(vw, force);
}

// 被控时主控解锁：
//   isControlled() 虚函数返回 true 时，UI 会禁用"连接"按钮，且 startRemoteAssist
//   点击后会直接 minimize 并跳过主控入口。
// 策略：hook isControlled() 始终返回 false，使按钮保持可用、主控正常启动。
// 安装时机：
//   1. install_hooks 时用版本表里的全局单例 RVA 立即尝试（覆盖按钮禁用场景）；
//   2. 首次点击"连接"（h_startRemoteAssist）时懒初始化兜底（版本未知或全局未就绪）。

// 被控（inbound）状态缓存：由 h_isCtrlCheck 实时更新。
static std::atomic<bool> g_isControlled{false};
// isControlled() 相关：提前声明供 h_initDevStatus 引用，定义在下方
using fn_is_ctrl_t = bool (__fastcall*)(void*);
static fn_is_ctrl_t o_isCtrlCheck   = nullptr;
static void*        g_isCtrlObj     = nullptr;
static uintptr_t    g_isCtrlGlobRva = 0;  // 保存供 h_initDevStatus 懒初始化重试
static bool         g_isCtrlGlobalDirect = false;
static bool         g_isCtrlMemberEmbedded = false;
static bool         g_isCtrlHookDone = false;
// 前向声明，定义在下方
static bool __fastcall h_isCtrlCheck(void* thiz);
// g_gvBase 定义在下方（debug 区域），此处先声明
extern uintptr_t g_gvBase;

// 辅助：调用 widget->vtable[11](0/1) 隐藏或显示 widget
static void vtable11_call(void* widget, int show) {
    if (!widget) return;
    __try {
        void* vtbl = *(void**)widget;
        using fn_t = void(__fastcall*)(void*, int);
        ((fn_t)(*(void**)((char*)vtbl + 0x58)))(widget, show);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void vtable11_hide(void* widget) { vtable11_call(widget, 0); }
static void vtable11_show(void* widget) { vtable11_call(widget, 1); }

// initDeviceStatus 在被控时会走禁用路径：
//   [rsi+0x130]/[rsi+0x138] 的叠加层被显示(vtable[11](1))，[rsi+0x30] 被置 3。
// 原函数返回后检查：若设备本身可控（status 正常 + controllable 标志非零）
// 却被设成禁用态，说明是被控导致——还原叠加层和状态，使"进入桌面"按钮恢复可用。
using fn_initDevStatus_t = void(__fastcall*)(void*, void*);
static fn_initDevStatus_t o_initDevStatus = nullptr;
static void __fastcall h_initDevStatus(void* thiz, void* data) {
    // 顺带懒初始化 isControlled() hook（供 startRemoteAssist 使用）
    if (!g_isCtrlHookDone && g_isCtrlGlobRva && g_gvBase) {
        __try {
            void* mgr = *(void**)(g_gvBase + g_isCtrlGlobRva);
            if (mgr) {
                void* nested = g_isCtrlGlobalDirect ? mgr
                             : g_isCtrlMemberEmbedded ? (void*)((char*)mgr + 0x30)
                                                      : *(void**)((char*)mgr + 0x30);
                if (nested) {
                    void* fn = (*(void***)nested)[0x140 / 8];
                    if (fn && MH_CreateHook(fn, (void*)h_isCtrlCheck, (void**)&o_isCtrlCheck) == MH_OK
                           && MH_EnableHook(fn) == MH_OK) {
                        g_isCtrlObj = nested;
                        g_isCtrlHookDone = true;
                        uu_log("isCtrlCheck lazy-retry @ %p (ok)", fn);
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    o_initDevStatus(thiz, data);
    // [thiz+0x108] 为 0 说明走了禁用路径（checkDeviceControllable 返回了 false）。
    // 这里只在确实观察到被控痕迹时做恢复，避免无 inbound 时把正常的禁用态也改写掉。
    if (*(unsigned char*)((char*)thiz + 0x108) != 0) return;
    __try {
        if (!data) return;
        void* dev = *(void**)data;   // 设备数据对象（data 是 DeviceData**）
        if (!dev) return;
        // status ∈ {3,4,5,6}：设备确实不可用（离线/忙/更新中）— 不干预
        if (((unsigned int)*(int*)((char*)dev + 0x28) - 3u) <= 3u) return;
        // [dev+0x48] controllable 标志为 0：平台限制，不干预
        if (*(unsigned char*)((char*)dev + 0x48) == 0) return;
        // 设备状态正常且标志可控，但走了禁用路径 → 只恢复已确认的进入桌面控件与可控状态位。
        uu_log("initDevStatus: restore enabled (inbound)");
        void* enterDesktop = *(void**)((char*)thiz + 0x110);
        vtable11_show(enterDesktop);                   // 显示进入桌面区域
        *(int*)((char*)thiz + 0x30) = 0;               // 清除禁用状态
        *(char*)((char*)thiz + 0x108) = 1;             // 可控标志
        *(char*)((char*)thiz + 0x50) = 1;              // 可控标志
        o_initDevStatus(thiz, data);                   // 让原始状态机再跑一遍，触发自然刷新
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// checkDeviceControllable hook：orig=false 但 status 正常且字符串非空 → 强制 true。
using fn_desktop_check_t = bool (__fastcall*)(void*);
static fn_desktop_check_t o_desktopCheck = nullptr;
static bool __fastcall h_desktopCheck(void* thiz) {
    bool orig = o_desktopCheck(thiz);
    if (orig) return true;
    __try {
        if (((unsigned int)*(int*)((char*)thiz + 0x28) - 3u) <= 3u) return false;
        void* d = *(void**)((char*)thiz + 0x58);
        if (!d || *(int*)((char*)d + 4) == 0) return false;
        uu_log("desktopCheck: forced enable (inbound)");
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// isControlled() 包装器 hook（fcn.140032489，thunk→0x1404e4d30）：
//   无参数，读全局单例虚表第 40 项。被控时返回 true，让各处 UI 禁用"进入桌面"/"文件传输"。
//   强制返回 false 使所有按钮状态恢复，覆盖 fcn.14003779a（按钮状态更新）内的被控判断。
using fn_isCtrlWrapper_t = bool(__fastcall*)();
static fn_isCtrlWrapper_t o_isCtrlWrapper = nullptr;
static bool __fastcall h_isCtrlWrapper() {
    return false;
}

using fn_start_ra_t = void (__fastcall*)(void*);
static fn_start_ra_t o_startRemoteAssist = nullptr;

static bool __fastcall h_isCtrlCheck(void* thiz) {
    g_isControlled.store(o_isCtrlCheck(thiz), std::memory_order_relaxed);
    return false;
}

// 从 nested 对象虚表第 40 项取 isControlled() 并挂钩
static bool hook_isctrl_from_nested(void* nested) {
    if (!nested) return false;
    void* fn = (*(void***)nested)[0x140 / 8];
    if (!fn) return false;
    if (MH_CreateHook(fn, (void*)h_isCtrlCheck, (void**)&o_isCtrlCheck) != MH_OK) return false;
    if (MH_EnableHook(fn) != MH_OK) return false;
    g_isCtrlObj = nested;
    g_isCtrlHookDone = true;
    uu_log("isCtrlCheck @ %p (ok)", fn);
    return true;
}

// 立即尝试：通过版本表中的全局单例 RVA 定位嵌套对象
static bool try_hook_isctrl_eager(uintptr_t base, uintptr_t globalRva, bool direct) {
    if (!globalRva) return false;
    __try {
        void* mgr = *(void**)(base + globalRva);
        if (!mgr) return false;
        void* nested = direct ? mgr
                 : g_isCtrlMemberEmbedded ? (void*)((char*)mgr + 0x30)
                              : *(void**)((char*)mgr + 0x30);
        return hook_isctrl_from_nested(nested);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        uu_log("try_hook_isctrl_eager: access violation");
        return false;
    }
}

static void __fastcall h_startRemoteAssist(void* thiz) {
    // 兜底懒初始化：若 install_hooks 时全局未就绪，首次点击时从 thiz 取嵌套对象
    if (!g_isCtrlHookDone) {
        __try {
            void* nested = g_isCtrlMemberEmbedded
                         ? (void*)((char*)thiz + 0x30)
                         : *(void**)((char*)thiz + 0x30);
            if (!hook_isctrl_from_nested(nested))
                uu_log("isCtrlCheck lazy-hook: failed");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            uu_log("isCtrlCheck lazy-hook: access violation");
        }
    }
    o_startRemoteAssist(thiz);
}

static bool g_verKnown = false;   // 当前 GameViewer 版本号是否在 offsets 表里

// 给托盘“调试信息”用：每个 hook 点的定位结果、模块基址、版本号
static std::mutex g_dbgMtx;
static std::vector<HookStat> g_hookStats;
static std::wstring g_gvVersion = L"?";
static uintptr_t g_gvBase = 0;

static void record_hook(const char* name, void* addr, const char* how, bool ok) {
    std::lock_guard<std::mutex> lk(g_dbgMtx);
    g_hookStats.push_back({ name, addr, how, ok });
}

// 版本认识就先用精确 RVA（字符串只做比对校验）；版本不认识就字符串 → AOB，绝不套别的版本的 RVA。
static bool mk(const resolver::ModRange& r, uintptr_t base, std::initializer_list<const char*> anchors,
               const ver::Target& t, void* detour, void** orig, const char* name) {
    uintptr_t byStr = anchors.size() ? resolver::find_func(r, anchors) : 0;
    uintptr_t tgt = 0; const char* how = "";
    if (g_verKnown && t.rva) {
        tgt = base + t.rva; how = "rva";
        if (byStr && byStr != tgt) uu_log("%s: str %p != rva %p, keep rva", name, (void*)byStr, (void*)tgt);
        else if (!byStr) uu_log("%s: string resolve missed (更新后会失效)", name);
    } else if (byStr) {
        tgt = byStr; how = "str";
    } else {
        uintptr_t byAob = resolver::find_func_by_aob(r, t.aob);
        if (byAob) { tgt = byAob; how = "aob"; }
    }
    if (!tgt) { uu_log("resolve %s failed, skip", name); record_hook(name, nullptr, "", false); return false; }
    if (MH_CreateHook((void*)tgt, detour, orig) != MH_OK) { uu_log("CreateHook %s failed", name); record_hook(name, (void*)tgt, how, false); return false; }
    if (MH_EnableHook((void*)tgt) != MH_OK) { uu_log("EnableHook %s failed", name); record_hook(name, (void*)tgt, how, false); return false; }
    uu_log("hooked %s @ %p (%s)", name, (void*)tgt, how);
    record_hook(name, (void*)tgt, how, true);
    return true;
}

void install_hooks(uintptr_t base) {
    if (MH_Initialize() != MH_OK) { uu_log("MH_Initialize failed"); return; }
    std::wstring vs = cfg::exe_version();
    const ver::VerSet& V = ver::pick(vs.c_str());
    g_verKnown = (!vs.empty() && vs == V.version);
    g_gvBase = base;
    g_gvVersion = vs.empty() ? L"?" : vs;
    CCS_DEVICE_ID_OFF = V.deviceIdOff;
    g_isCtrlGlobRva = V.isCtrlGlobalRva.rva;
    g_isCtrlGlobalDirect = V.isCtrlGlobalDirect;
    g_isCtrlMemberEmbedded = V.isCtrlMemberEmbedded;
    uu_log("GameViewer version=%ls known=%d", vs.empty() ? L"?" : vs.c_str(), (int)g_verKnown);
    resolver::ModRange r{};
    resolver::get_ranges((HMODULE)base, r);
    mk(r, base, {"ControlConnectionSession::sendMouseEvent", "[control] mouseObj size 0"},      V.sendMouse,    (void*)h_sendMouse, (void**)&o_sendMouse, "sendMouseEvent");
    mk(r, base, {"ControlConnectionSession::sendMouseWheel", "sendMouseWheel failed, session_config_ handle invalid"}, V.sendWheel, (void*)h_sendWheel, (void**)&o_sendWheel, "sendMouseWheel");
    mk(r, base, {"ControlConnectionSession::sendKeyboardEvent"}, V.sendKey, (void*)h_sendKey,   (void**)&o_sendKey,   "sendKeyboardEvent");
    mk(r, base, {"VideoUi::VideoWidget::enabledCaptureMouse", "==== Enabled capture mouse: ", "Cursor not in rect"}, V.enableCapture, (void*)h_enableCapture, (void**)&o_enableCapture, "enabledCaptureMouse");
    mk(r, base, {"GamepadManager::Connect(), index=", "GamepadManager::Connect"},       V.gpConnect,    (void*)h_gamepadConnect,    (void**)&o_gamepadConnect,    "GamepadManager::Connect");
    mk(r, base, {"GamepadManager::Disconnect(), index=", "GamepadManager::Disconnect"}, V.gpDisconnect, (void*)h_gamepadDisconnect, (void**)&o_gamepadDisconnect, "GamepadManager::Disconnect");
    mk(r, base, {"[%d] GamepadManager::Update(), json=%s", "GamepadManager::Update"},    V.gpUpdate,   (void*)h_gamepadUpdate,     (void**)&o_gamepadUpdate,     "GamepadManager::Update");
    mk(r, base, {"Clipboard::on_clipboard_update", "Get clipboard data failed"},         V.clipUpdate, (void*)h_clipUpdate, (void**)&o_clipUpdate, "on_clipboard_update");
    mk(r, base, {"Clipboard::do_handle_format_list_request", "do_handle_format_list_request: is_file_transferring=true"}, V.clipFmtList, (void*)h_clipFmtList, (void**)&o_clipFmtList, "do_handle_format_list_request");
    mk(r, base, {"Clipboard::get_clipboard_data", "GlobalLock failed: "}, V.clipGet, (void*)h_clipGet, (void**)&o_clipGet, "get_clipboard_data");
    mk(r, base, {"Clipboard::do_send_format_list", "do_send_format_list: send_request failed"}, V.clipSendFmt, (void*)h_clipSendFmt, (void**)&o_clipSendFmt, "do_send_format_list");
    // 会话注册/移除
    mk(r, base, {"ControlConnectionSession::setConnectInfo", "startConnectOtherDevice, device_id: "}, V.setConnInfo, (void*)h_setConnInfo, (void**)&o_setConnInfo, "setConnectInfo");
    mk(r, base, {"ControlConnectionSession::closeControlConnect"}, V.closeConn, (void*)h_closeConn,   (void**)&o_closeConn,   "closeControlConnect");
    mk(r, base, {"ControlConnectionSession::exitRoom"},            V.exitRoom,  (void*)h_exitRoom,    (void**)&o_exitRoom,    "exitRoom");
    // 光标
    mk(r, base, {"VideoUi::VideoWidget::updateCursor", "set cursor by id", "Default set arrow cursor"}, V.updateCursor, (void*)h_updateCursor, (void**)&o_updateCursor, "updateCursor");
    // 被控时主控解锁
    mk(r, base, {"NewUi::HomePageContent::startRemoteAssist", "startRemoteAssist: self is controlled"},
       V.startRemoteAssist, (void*)h_startRemoteAssist, (void**)&o_startRemoteAssist, "startRemoteAssist");
    // 被控时"进入桌面"按钮解锁：
    //   initDeviceStatus 用于懒初始化 isControlled() hook（让 desktopCheck 能查到它）
    //   desktopCheck (checkDeviceControllable)：被控时将 false 强制改为 true，进入启用路径
    mk(r, base, {"Device desktop is disabled, id: "}, V.initDevStatus, (void*)h_initDevStatus, (void**)&o_initDevStatus, "initDeviceStatus");
    mk(r, base, {}, V.desktopCheck, (void*)h_desktopCheck, (void**)&o_desktopCheck, "desktopCheck");
    // isControlled() 包装器：强制返回 false，覆盖按钮禁用逻辑（含 文件传输/进入桌面 底部按钮）
    mk(r, base, {}, V.isCtrlWrapper, (void*)h_isCtrlWrapper, (void**)&o_isCtrlWrapper, "isCtrlWrapper");
    // 立即尝试挂钩 isControlled()，使"连接"按钮在被控时保持启用
    // 若全局单例尚未就绪（返回 false），h_initDevStatus/h_startRemoteAssist 会懒初始化兜底
    if (!try_hook_isctrl_eager(base, V.isCtrlGlobalRva.rva, V.isCtrlGlobalDirect))
        uu_log("isCtrlCheck: eager hook deferred (global not ready or RVA unknown)");
    uu_log("install_hooks done");
}

DebugInfo debug_snapshot() {
    std::lock_guard<std::mutex> lk(g_dbgMtx);
    DebugInfo d;
    d.gvVersion = g_gvVersion;
    d.gvKnown = g_verKnown;
    d.gvBase = g_gvBase;
    d.hooks = g_hookStats;
    return d;
}
