#include <windows.h>
#include <set>
#include <map>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <iterator>
#include <cstdint>
#include <cwctype>
#include <intrin.h>
#include "MinHook.h"
#include "offsets.h"
#include "config.h"
#include "log.h"
#include "resolver.h"
#include "xdisasm.h"
#include "hookset.h"
#include "session.h"
#include "srvdbg.h"

using fn_send_t   = void*(__fastcall*)(void*, void*, void*, void*, void*, void*, void*, void*);
using fn_cap_t    = void (__fastcall*)(void* thiz, unsigned __int8 enable, char toast, char a4);
using fn_clipupd_t= void (__fastcall*)(void* thiz);
using fn_fmtlist_t= __int64(__fastcall*)(void* thiz, void* a2, void* a3);
using fn_clipget_t= __int64(__fastcall*)(void* hwnd, unsigned int fmt, void* out);
using fn_clipget436_t = void(__fastcall*)(void* hwnd, unsigned int fmt, void* out, void* mutex);
using fn_sendfmt_t= __int64(__fastcall*)(void* thiz);
using fn_clipreq_t= __int64(__fastcall*)(void* thiz, void* a2, void* a3);
using fn_gpupd_t  = void (__fastcall*)(void* thiz, void* padState);
using fn_vmwctor_t= __int64(__fastcall*)(void* thiz, void* devidQs, void* a3, void* sp, int a5, __int64 a6);
using fn_vmwctor436_t = __int64(__fastcall*)(void* thiz, void* devidQs, void* coverQs, int mode, __int64 parent);
using fn_vmwclose436_t = char(__fastcall*)(void* thiz, void* event);
using fn_vmwdtor436_t = void(__fastcall*)(void* thiz);
using fn_capture436_t = char(__fastcall*)(void* thiz, const char* source);

static fn_send_t    o_sendMouse = nullptr, o_sendWheel = nullptr, o_sendKey = nullptr;
static fn_cap_t     o_enableCapture = nullptr;
static fn_clipupd_t o_clipUpdate = nullptr;
static fn_fmtlist_t o_clipFmtList = nullptr;
static fn_clipget_t o_clipGet = nullptr;
static fn_clipget436_t o_clipGet436 = nullptr;
static fn_sendfmt_t o_clipSendFmt = nullptr;
static fn_clipreq_t o_clipReq = nullptr;
static fn_gpupd_t   o_gamepadUpdate = nullptr, o_gamepadConnect = nullptr, o_gamepadDisconnect = nullptr;
static fn_vmwctor_t o_vmwCtor = nullptr;
static fn_vmwctor436_t o_vmwCtor436 = nullptr;
static fn_vmwclose436_t o_vmwClose436 = nullptr;
static fn_vmwdtor436_t o_vmwDtor436 = nullptr;
static fn_capture436_t o_capture436 = nullptr;

using fn_lock_t = BOOL(WINAPI*)(void);
static fn_lock_t o_lockWorkStation = nullptr;
static BOOL WINAPI h_lockWorkStation(void) {
    if (cfg::srv_block(cfg::SF_PRIVACY)) { uu_log("view-only: blocked LockWorkStation"); return TRUE; }
    return o_lockWorkStation();
}

static uintptr_t CCS_DEVICE_ID_OFF = 3984;
static uintptr_t VMW_DEVICE_ID_OFF = 344;
static uintptr_t VMW_TITLE_OFF     = 352;
static uintptr_t ISCTRL_VT_SLOT_OFF = 0x140;
static bool      g_devIdAuto  = false;
static bool      g_vmwOffAuto = false;

struct SessState { bool viewOnly; bool clipSync; bool gamepadOff; std::wstring devid; };
static std::mutex                 g_smtx;
static std::map<void*, SessState> g_sessions;
static void*                      g_activeCCS = nullptr;

static std::map<std::wstring, void*>        g_devidToVmw;
static std::map<std::wstring, std::wstring> g_devidToTitle;

static void*                      g_serverClip = nullptr;
static std::map<void*, void*>     g_clipToCcs;
static thread_local void*         t_curClip = nullptr;

static int safe_copy_devid(void* ccs, char* buf, int bufsz) {
    __try {
        char* s = (char*)ccs + CCS_DEVICE_ID_OFF;
        size_t len = *(size_t*)(s + 16);
        size_t cap = *(size_t*)(s + 24);
        const char* p = (cap >= 16) ? *(const char**)s : s;
        if (!p || len == 0 || len >= (size_t)bufsz) return 0;
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

static std::wstring device_key(const std::wstring& devid) {
    size_t first = 0, last = devid.size();
    while (first < last && iswspace(devid[first])) ++first;
    while (last > first && iswspace(devid[last - 1])) --last;
    std::wstring key = devid.substr(first, last - first);
    for (wchar_t& c : key) c = (wchar_t)towlower(c);
    return key;
}

// 调用方必须持有 g_smtx。
static void session_remove_locked(void* ccs, const char* reason) {
    auto it = g_sessions.find(ccs);
    if (it != g_sessions.end()) {
        g_sessions.erase(it);
        uu_log("session remove: ccs=%p reason=%s", ccs, reason ? reason : "unknown");
    }
    if (g_activeCCS == ccs) g_activeCCS = nullptr;
    for (auto jt = g_clipToCcs.begin(); jt != g_clipToCcs.end(); )
        jt = (jt->second == ccs) ? g_clipToCcs.erase(jt) : std::next(jt);
}

// QString layout: d ptr -> QArrayData [+4]int size [+16]qptrdiff offset; chars at (char*)d+offset
static int safe_copy_qstr(const void* qsHolder, wchar_t* buf, int cap) {
    __try {
        const unsigned char* d = *(const unsigned char* const*)qsHolder;
        if (!d) return 0;
        int size = *(const int*)(d + 4);
        long long off = *(const long long*)(d + 16);
        if (size <= 0 || size >= cap) return 0;
        const unsigned short* s = (const unsigned short*)(d + off);
        for (int i = 0; i < size; ++i) {
            unsigned short c = s[i];
            if (c == 0) return i;
            buf[i] = (wchar_t)c;
        }
        return size;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static std::wstring read_qstring(const void* qsHolder) {
    wchar_t buf[256];
    int n = safe_copy_qstr(qsHolder, buf, 256);
    if (n <= 0) return L"";
    return std::wstring(buf, n);
}

static uintptr_t derive_off_after_str(const resolver::ModRange& r, uintptr_t func, const char* anchorStr) {
    if (!func) return 0;
    uintptr_t sa = resolver::find_string(r, anchorStr);
    if (!sa) return 0;
    const uint8_t* p0 = (const uint8_t*)func;
    const uint8_t* end = p0 + 0x400;
    if ((uintptr_t)end > r.text_end) end = (const uint8_t*)r.text_end;
    const uint8_t* after = nullptr;
    for (const uint8_t* q = p0; q < end; ) {
        xd::Insn i = xd::decode(q);
        if (!i.len) { ++q; continue; }
        if (i.opcode == 0x8D && !i.two_byte && i.rip_rel && xd::rip_target(q, i) == sa) { after = q + i.len; break; }
        q += i.len;
    }
    if (!after) return 0;
    const uint8_t* s2end = after + 0x40;
    if ((uintptr_t)s2end > (uintptr_t)end) s2end = end;
    for (const uint8_t* q = after; q < s2end; ) {
        xd::Insn i = xd::decode(q);
        if (!i.len) { ++q; continue; }
        if (i.opcode == 0x8D && !i.two_byte && i.has_modrm && i.mod == 2 && i.rm != 4 && i.rm != 5) {
            int32_t disp = i.disp;
            if (disp >= 0x40 && disp <= 0x8000) return (uintptr_t)disp;
        }
        q += i.len;
    }
    return 0;
}

// handleKeyEvent 内定位 sub_1406A8120(普通键转发+置消费)：两分支各以
// lea rcx,[rsp+disp];call 调它一次，取出现≥2 次的 call 目标。
static void* find_raw_key_forward(const resolver::ModRange& r, uintptr_t hke) {
    if (!hke) return nullptr;
    const uint8_t* p0 = (const uint8_t*)hke;
    const uint8_t* end = p0 + 0x680;
    if ((uintptr_t)end > r.text_end) end = (const uint8_t*)r.text_end;
    std::map<uintptr_t, int> tally;
    bool argReady = false; int gap = 0;
    for (const uint8_t* q = p0; q < end; ) {
        xd::Insn i = xd::decode(q);
        if (!i.len) { ++q; argReady = false; continue; }
        bool leaRcxRsp = i.opcode == 0x8D && !i.two_byte && i.has_modrm && i.reg == 1 && !i.rex_r
                      && !i.rip_rel && i.has_sib && i.sib_base == 4 && i.sib_index == 4 && !i.rex_b && !i.rex_x;
        if (leaRcxRsp) { argReady = true; gap = 0; }
        else if (argReady && i.opcode == 0xE8 && !i.two_byte && i.has_rel) {
            uintptr_t tgt = xd::rel_target(q, i);
            if (tgt >= r.text_beg && tgt < r.text_end) tally[tgt]++;
            argReady = false;
        }
        else if (argReady && ++gap > 1) argReady = false;
        q += i.len;
    }
    uintptr_t best = 0; int bestc = 0;
    for (auto& kv : tally) if (kv.second > bestc) { best = kv.first; bestc = kv.second; }
    return bestc >= 2 ? (void*)best : nullptr;
}

static bool g_vmwDerived = false;
static void derive_vmw_off(void* thiz, const void* devidQs) {
    std::wstring want = read_qstring(devidQs);
    if (want.empty()) return;
    for (uintptr_t off = 0x100; off <= 0x400; off += 8)
        if (read_qstring((char*)thiz + off) == want) {
            VMW_DEVICE_ID_OFF = off;
            // 4.36+ title 不再紧贴 devid(+8)，而是隔若干空 QString；
            // 向后扫描第一个非空 QString 作为标题。
            uintptr_t tit = off + 8;
            for (uintptr_t t = off + 8; t <= off + 0x80; t += 8)
                if (!read_qstring((char*)thiz + t).empty()) { tit = t; break; }
            VMW_TITLE_OFF = tit;
            g_vmwOffAuto = true;
            uu_log("vmw offsets auto-derived: devid=+%llu title=+%llu",
                   (unsigned long long)off, (unsigned long long)tit);
            return;
        }
    uu_log("vmw offsets auto-derive missed, keep table devid=+%llu", (unsigned long long)VMW_DEVICE_ID_OFF);
}

static SessState& sessOf(void* ccs) {
    auto it = g_sessions.find(ccs);
    if (it != g_sessions.end()) return it->second;
    SessState s;
    s.viewOnly   = cfg::g_viewOnly.load();
    s.clipSync   = cfg::g_clipSync.load();
    s.gamepadOff = cfg::g_gamepadOff.load();
    s.devid       = read_device_id(ccs);
    // 4.36 的旧窗口偶尔晚于新连接释放。同一设备开始发送输入时，旧 CCS 已不再是
    // 可操作会话，先清理它，保证托盘最多只保留一个当前记录。
    const std::wstring key = device_key(s.devid);
    if (!key.empty()) {
        for (auto old = g_sessions.begin(); old != g_sessions.end(); ) {
            if (old->first != ccs && device_key(old->second.devid) == key) {
                void* stale = old->first;
                ++old;
                session_remove_locked(stale, "replaced_same_device");
            } else {
                ++old;
            }
        }
    }
    uu_log("session new: ccs=%p devid=%ls viewOnly=%d", ccs, s.devid.c_str(), (int)s.viewOnly);
    return g_sessions.emplace(ccs, std::move(s)).first->second;
}
static bool input_viewOnly(void* ccs) {
    std::lock_guard<std::mutex> lk(g_smtx);
    g_activeCCS = ccs;
    return sessOf(ccs).viewOnly;
}
static bool active_viewOnly() {
    std::lock_guard<std::mutex> lk(g_smtx);
    if (!g_activeCCS) return cfg::g_viewOnly.load();
    return sessOf(g_activeCCS).viewOnly;
}
static bool clip_allowed_locked(void* clip) {
    if (!clip) {
        if (g_activeCCS) return sessOf(g_activeCCS).clipSync;
        return true;
    }
    if (!g_activeCCS) {
        g_serverClip = clip;
        g_clipToCcs.erase(clip);
    }
    if (clip == g_serverClip) return cfg::g_ctrlClip.load() && !cfg::g_srvViewOnly.load();
    auto it = g_clipToCcs.find(clip);
    void* ccs = (it != g_clipToCcs.end()) ? it->second : nullptr;
    if (!ccs && g_activeCCS) { g_clipToCcs[clip] = g_activeCCS; ccs = g_activeCCS; }
    if (ccs) { auto s = g_sessions.find(ccs); if (s != g_sessions.end()) return s->second.clipSync; }
    return true;
}
static bool clip_allowed(void* clip) {
    std::lock_guard<std::mutex> lk(g_smtx);
    return clip_allowed_locked(clip);
}
static bool active_gpBlock() {
    std::lock_guard<std::mutex> lk(g_smtx);
    if (!g_activeCCS) return cfg::g_viewOnly.load() || cfg::g_gamepadOff.load();
    auto& s = sessOf(g_activeCCS);
    return s.viewOnly || s.gamepadOff;
}

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
static void __fastcall h_enableCapture(void* thiz, unsigned __int8 enable, char toast, char a4) {
    if (active_viewOnly()) enable = 0;   // 仅浏览：不进捕获
    o_enableCapture(thiz, enable, toast, a4);
}

// 4.36 的 enabledCaptureMouse 已拆成 captureMouse(source)，不再接收 enable/toast。
// 直接拒绝进入捕获；释放路径和模式状态机仍走原实现。
static char __fastcall h_capture436(void* thiz, const char* source) {
    if (active_viewOnly()) return 0;
    return o_capture436(thiz, source);
}

// sub_1406A8120：handleKeyEvent 里普通键"转发+置消费"的分支，ctx[4] 是消费标志。
// 仅浏览时置 0 且不转发 → 键落回本机 OS(Alt+Tab 生效)；UU 快捷键不经此，不受影响。
using fn_rawfwd_t = void*(__fastcall*)(void**, void*, void*, void*);
static fn_rawfwd_t o_rawKeyForward = nullptr;
static void* __fastcall h_rawKeyForward(void** ctx, void* a2, void* a3, void* a4) {
    if (active_viewOnly()) {
        if (ctx) { auto* consume = (unsigned char*)ctx[4]; if (consume) *consume = 0; }
        return ctx ? (void*)ctx[4] : nullptr;
    }
    return o_rawKeyForward(ctx, a2, a3, a4);
}

static void __fastcall h_clipUpdate(void* thiz) {
    if (!clip_allowed(thiz)) return;
    void* prev = t_curClip; t_curClip = thiz;
    o_clipUpdate(thiz);
    t_curClip = prev;
}
static __int64 __fastcall h_clipSendFmt(void* thiz) {
    if (!clip_allowed(thiz)) return 0;
    return o_clipSendFmt(thiz);
}
static __int64 __fastcall h_clipFmtList(void* thiz, void* a2, void* a3) {
    if (!clip_allowed(thiz)) return 0;
    return o_clipFmtList(thiz, a2, a3);
}
static __int64 __fastcall h_clipReq(void* thiz, void* a2, void* a3) {
    void* prev = t_curClip; t_curClip = thiz;
    __int64 r = o_clipReq(thiz, a2, a3);
    t_curClip = prev;
    return r;
}
// out is MSVC std::string: [0..15]SSO/ptr [16]size [24]cap
static __int64 __fastcall h_clipGet(void* hwnd, unsigned int fmt, void* out) {
    if (!clip_allowed(t_curClip)) {
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

// 4.36 新增第四个 mutex 参数且返回 void。旧签名调用会丢 R9 并在原函数内崩溃，必须分开 Hook。
static void __fastcall h_clipGet436(void* hwnd, unsigned int fmt, void* out, void* mutex) {
    if (!clip_allowed(t_curClip)) {
        if (out) {
            size_t* s = (size_t*)out;
            char* buf = s[3] >= 0x10 ? *(char**)out : (char*)out;
            s[2] = 0;
            buf[0] = 0;
        }
        return;
    }
    o_clipGet436(hwnd, fmt, out, mutex);
}

static std::mutex        g_gpMtx;
static void*             g_gpMgr = nullptr;
static std::set<uint8_t> g_desired;
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

static __int64 __fastcall h_vmwCtor(void* thiz, void* devidQs, void* a3, void* sp, int a5, __int64 a6) {
    __int64 r = o_vmwCtor(thiz, devidQs, a3, sp, a5, a6);
    if (!g_vmwDerived) { g_vmwDerived = true; derive_vmw_off(thiz, devidQs); }
    std::wstring devid = read_qstring((char*)thiz + VMW_DEVICE_ID_OFF);
    if (!devid.empty()) {
        std::lock_guard<std::mutex> lk(g_smtx);
        const std::wstring key = device_key(devid);
        g_devidToVmw[key] = thiz;
        std::wstring title = read_qstring((char*)thiz + VMW_TITLE_OFF);
        if (!title.empty()) g_devidToTitle[key] = std::move(title);
        uu_log("vmw registered: devid=%ls vmw=%p", devid.c_str(), thiz);
    }
    return r;
}


static __int64 __fastcall h_vmwCtor436(void* thiz, void* devidQs, void* coverQs, int mode, __int64 parent) {
    // 4.36 构造函数的第三个 QString 是桌面封面图 URL，不是会话名称。
    // 真正的标题在连接完成时写入 VideoMainWindow + 0x1E8，快照时从该字段读取。
    std::wstring ctorDevid = read_qstring(devidQs);
    __int64 r = o_vmwCtor436(thiz, devidQs, coverQs, mode, parent);
    std::wstring devid = std::move(ctorDevid);
    if (devid.empty()) devid = read_qstring((char*)thiz + VMW_DEVICE_ID_OFF);
    if (!devid.empty()) {
        std::lock_guard<std::mutex> lk(g_smtx);
        const std::wstring key = device_key(devid);
        g_devidToVmw[key] = thiz;
        std::wstring title = read_qstring((char*)thiz + VMW_TITLE_OFF);
        if (!title.empty()) g_devidToTitle[key] = title;
        uu_log("vmw registered: devid=%ls title=%ls vmw=%p", devid.c_str(), title.c_str(), thiz);
    }
    return r;
}

static void session_remove_vmw436(void* vmw, const char* reason) {
    std::wstring devid = read_qstring((char*)vmw + VMW_DEVICE_ID_OFF);
    const std::wstring key = device_key(devid);
    if (key.empty()) return;

    std::lock_guard<std::mutex> lk(g_smtx);
    auto mapped = g_devidToVmw.find(key);
    // 旧窗口可能在同设备的新窗口创建后才析构，不能误删新会话。
    if (mapped != g_devidToVmw.end() && mapped->second != vmw) {
        uu_log("vmw cleanup skipped: stale vmw=%p devid=%ls", vmw, devid.c_str());
        return;
    }

    int removed = 0;
    for (auto it = g_sessions.begin(); it != g_sessions.end(); ) {
        std::wstring sessionDevid = it->second.devid;
        if (sessionDevid.empty()) sessionDevid = read_device_id(it->first);
        if (device_key(sessionDevid) == key) {
            void* ccs = it->first;
            ++it;
            session_remove_locked(ccs, reason);
            ++removed;
        } else {
            ++it;
        }
    }
    if (mapped != g_devidToVmw.end()) g_devidToVmw.erase(mapped);
    g_devidToTitle.erase(key);
    uu_log("vmw cleanup: vmw=%p devid=%ls removed=%d reason=%s",
           vmw, devid.c_str(), removed, reason ? reason : "unknown");
}

static char __fastcall h_vmwClose436(void* thiz, void* event) {
    session_remove_vmw436(thiz, "window_closed");
    return o_vmwClose436(thiz, event);
}

static void __fastcall h_vmwDtor436(void* thiz) {
    session_remove_vmw436(thiz, "window_destroyed");
    o_vmwDtor436(thiz);
}

std::vector<SessSnap> sessions_snapshot() {
    std::lock_guard<std::mutex> lk(g_smtx);
    std::vector<SessSnap> v;
    for (auto& kv : g_sessions) {
        // 4.36 没有旧版 setConnectInfo 生命周期钩子；首次输入时 id 可能尚未就绪，快照时刷新。
        std::wstring liveDevid = read_device_id(kv.first);
        if (!liveDevid.empty()) kv.second.devid = std::move(liveDevid);
        std::wstring disp = kv.second.devid;
        const std::wstring key = device_key(kv.second.devid);
        auto titleIt = g_devidToTitle.find(key);
        if (titleIt != g_devidToTitle.end() && !titleIt->second.empty()) disp = titleIt->second;

        auto it = g_devidToVmw.find(key);
        if (!key.empty() && it != g_devidToVmw.end()) {
            void* vmw = it->second;
            std::wstring vd = read_qstring((char*)vmw + VMW_DEVICE_ID_OFF);
            if (device_key(vd) == key) {
                std::wstring title = read_qstring((char*)vmw + VMW_TITLE_OFF);
                if (!title.empty()) {
                    disp = title;
                    g_devidToTitle[key] = std::move(title);
                }
            }
        }
        v.push_back({ kv.first, disp, kv.second.viewOnly, kv.second.clipSync, kv.second.gamepadOff });
    }
    return v;
}
// field: 0=viewOnly 1=clipSync 2=gamepadOff
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
    if (field == 0 && nv) ClipCursor(nullptr);
    if (doGpReconcile) gp_reconcile(block);
    uu_log("session_toggle key=%p field=%d -> %d", key, field, (int)nv);
    return nv;
}

using fn4_t = __int64(__fastcall*)(void*, void*, void*, void*);
static fn4_t o_setConnInfo = nullptr, o_closeConn = nullptr, o_exitRoom = nullptr;

static void session_remove(void* ccs) {
    std::lock_guard<std::mutex> lk(g_smtx);
    session_remove_locked(ccs, "connection_closed");
}
static __int64 __fastcall h_setConnInfo(void* ccs, void* a2, void* a3, void* a4) {
    {
        std::lock_guard<std::mutex> lk(g_smtx);
        std::wstring devid = read_device_id(ccs);
        if (!devid.empty())
            for (auto it = g_sessions.begin(); it != g_sessions.end(); )
                it = (it->first != ccs && it->second.devid == devid) ? g_sessions.erase(it) : std::next(it);
        g_activeCCS = ccs;
        sessOf(ccs);
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

using fn_curs_t = __int64(__fastcall*)(void*, unsigned int);
static fn_curs_t o_updateCursor = nullptr;
static __int64 __fastcall h_updateCursor(void* vw, unsigned int force) {
    if (active_viewOnly()) {
        SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_NO));
        return 1;
    }
    return o_updateCursor(vw, force);
}
using fn_curs436_t = char(__fastcall*)(void*);
static fn_curs436_t o_updateCursor436 = nullptr;
static fn_curs436_t o_cursorApply436 = nullptr;

static void set_viewonly_cursor436() {
    SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_NO));
}

static char __fastcall h_updateCursor436(void* vw) {
    if (active_viewOnly()) {
        set_viewonly_cursor436();
        return 1;
    }
    return o_updateCursor436(vw);
}

// VideoWidget 最终的光标应用入口。即使上游已设置禁用光标，WM_SETCURSOR 路径仍可能
// 执行 SetCursor(NULL) 将其隐藏，因此仅浏览模式必须在这一层持续覆盖。
static char __fastcall h_cursorApply436(void* vw) {
    if (active_viewOnly()) {
        set_viewonly_cursor436();
        return 1;
    }
    return o_cursorApply436(vw);
}

// 被控时仍允许本机作为主控。UI 的禁用状态走此包装器。
using fn_isCtrlWrapper_t = bool(__fastcall*)();
static fn_isCtrlWrapper_t o_isCtrlWrapper = nullptr;
static fn_isCtrlWrapper_t o_isCtrlDirect = nullptr;
static bool __fastcall h_isCtrlWrapper() { return false; }

using fn_start_ra_t = void (__fastcall*)(void*);
static fn_start_ra_t o_startRemoteAssist = nullptr;
using fn_is_ctrl_t = bool (__fastcall*)(void*);
static fn_is_ctrl_t o_isCtrlCheck = nullptr;
static std::atomic<bool> g_isCtrlHookDone{false};
static std::atomic_flag g_isCtrlHooking = ATOMIC_FLAG_INIT;
static uintptr_t g_isCtrlMemberOff = 0;
static bool g_isCtrlMemberEmbedded = false;

static bool __fastcall h_isCtrlCheck(void*) { return false; }

static bool hook_isctrl_from_home(void* home) {
    if (!home || !g_isCtrlMemberOff) return false;
    if (g_isCtrlHookDone.load(std::memory_order_acquire)) return true;
    if (g_isCtrlHooking.test_and_set(std::memory_order_acquire))
        return g_isCtrlHookDone.load(std::memory_order_acquire);
    bool ok = false;
    __try {
        void* member = (char*)home + g_isCtrlMemberOff;
        void* state = g_isCtrlMemberEmbedded
                    ? member
                    : *(void**)member;
        void* target = state ? (*(void***)state)[ISCTRL_VT_SLOT_OFF / sizeof(void*)] : nullptr;
        if (target && MH_CreateHook(target, (void*)h_isCtrlCheck, (void**)&o_isCtrlCheck) == MH_OK) {
            if (MH_EnableHook(target) == MH_OK) {
                g_isCtrlHookDone.store(true, std::memory_order_release);
                uu_log("isControlled virtual @ %p (ok)", target);
                ok = true;
            } else {
                MH_RemoveHook(target);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        uu_log("isControlled virtual: access violation");
    }
    g_isCtrlHooking.clear(std::memory_order_release);
    return ok;
}

static void __fastcall h_startRemoteAssist(void* thiz) {
    if (!hook_isctrl_from_home(thiz)) uu_log("isControlled virtual hook failed");
    o_startRemoteAssist(thiz);
}

// 4.36 有两个必须绕过的直接 isControlled() 调用：
//   1) 提交新设备连接；2) startRemoteAssist 冲突保护。
// 仅按返回地址放行这两处，收起/展开被控页等其它调用仍返回真实状态。
using fn_is_ctrl436_t = bool(__fastcall*)(void*);
static fn_is_ctrl436_t o_isCtrlNarrow436 = nullptr;
static uintptr_t g_isCtrlConnectRet436 = 0;
static uintptr_t g_isCtrlGuardRet436 = 0;
static bool __fastcall h_isCtrlNarrow436(void* thiz) {
    const uintptr_t ret = (uintptr_t)_ReturnAddress();
    if (ret == g_isCtrlConnectRet436 || ret == g_isCtrlGuardRet436) return false;
    return o_isCtrlNarrow436(thiz);
}

// DeviceDesktopScene::render(DeviceDetailViewData const&) 同时检查：
//   data+0x69 当前设备允许控制；data+0x6a 本机正被控制。
// 仅置 0x69=1 仍会被第二项判为禁用。只在同步渲染期间临时改成“允许且未被控”，
// 不改共享设备模型，也不影响被控窗口的收起状态。
using fn_device_scene_render436_t = void(__fastcall*)(void*, unsigned char*);
static fn_device_scene_render436_t o_deviceSceneRender436 = nullptr;
static uintptr_t g_deviceControlAllowedOff436 = 0;
static uintptr_t g_deviceControlledOff436 = 0;
static void __fastcall h_deviceSceneRender436(void* scene, unsigned char* data) {
    unsigned char* allowed = nullptr;
    unsigned char* controlled = nullptr;
    unsigned char savedAllowed = 0;
    unsigned char savedControlled = 0;
    __try {
        if (data && g_deviceControlAllowedOff436 && g_deviceControlledOff436) {
            const unsigned int platform = *(unsigned int*)(data + 0x60);
            if (platform == 1 || platform == 4) {
                allowed = data + g_deviceControlAllowedOff436;
                controlled = data + g_deviceControlledOff436;
                savedAllowed = *allowed;
                savedControlled = *controlled;
                *allowed = 1;
                *controlled = 0;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        allowed = nullptr;
        controlled = nullptr;
    }

    o_deviceSceneRender436(scene, data);

    if (allowed && controlled) {
        __try {
            *allowed = savedAllowed;
            *controlled = savedControlled;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

// 仅浏览时不让光标被锁进视频窗口。游戏相对模式会持续 ClipCursor 锁回，故须 hook 持续拦，而非一次释放。
using fn_clip_t = BOOL(WINAPI*)(const RECT*);
static fn_clip_t o_ClipCursor = nullptr;
static BOOL WINAPI h_ClipCursor(const RECT* rc) {
    if (rc && active_viewOnly()) return o_ClipCursor(nullptr);
    return o_ClipCursor(rc);
}

static bool g_verKnown = false;

static std::mutex g_dbgMtx;
static std::vector<HookStat> g_hookStats;
static std::wstring g_gvVersion = L"?";
static uintptr_t g_gvBase = 0;

static void record_hook(const char* name, void* addr, const char* how, bool ok) {
    std::lock_guard<std::mutex> lk(g_dbgMtx);
    g_hookStats.push_back({ name, addr, how, ok });
}

struct InProcRecorder : hookset::IRecorder {
    void record(const char* name, void* addr, const char* how, bool ok) override {
        record_hook(name, addr, how, ok);
    }
};

static const hookset::Hook kLegacyHooks[] = {
    { "sendMouseEvent",              { "ControlConnectionSession::sendMouseEvent", "[control] mouseObj size 0", nullptr, nullptr }, (void*)h_sendMouse, (void**)&o_sendMouse },
    { "sendMouseWheel",              { "ControlConnectionSession::sendMouseWheel", "sendMouseWheel failed, session_config_ handle invalid", nullptr, nullptr }, (void*)h_sendWheel, (void**)&o_sendWheel },
    { "sendKeyboardEvent",           { "ControlConnectionSession::sendKeyboardEvent", nullptr, nullptr, nullptr }, (void*)h_sendKey, (void**)&o_sendKey },
    { "enabledCaptureMouse",         { "VideoUi::VideoWidget::enabledCaptureMouse", "==== Enabled capture mouse: ", "Cursor not in rect", nullptr }, (void*)h_enableCapture, (void**)&o_enableCapture },
    { "GamepadManager::Connect",     { "GamepadManager::Connect(), index=", "GamepadManager::Connect", nullptr, nullptr }, (void*)h_gamepadConnect, (void**)&o_gamepadConnect },
    { "GamepadManager::Disconnect",  { "GamepadManager::Disconnect(), index=", "GamepadManager::Disconnect", nullptr, nullptr }, (void*)h_gamepadDisconnect, (void**)&o_gamepadDisconnect },
    { "GamepadManager::Update",      { "[%d] GamepadManager::Update(), json=%s", "GamepadManager::Update", nullptr, nullptr }, (void*)h_gamepadUpdate, (void**)&o_gamepadUpdate },
    { "on_clipboard_update",         { "Clipboard::on_clipboard_update", "Get clipboard data failed", nullptr, nullptr }, (void*)h_clipUpdate, (void**)&o_clipUpdate },
    { "do_handle_format_list_request",{ "Clipboard::do_handle_format_list_request", "do_handle_format_list_request: is_file_transferring=true", nullptr, nullptr }, (void*)h_clipFmtList, (void**)&o_clipFmtList },
    { "get_clipboard_data",          { "Clipboard::get_clipboard_data", "GlobalLock failed: ", nullptr, nullptr }, (void*)h_clipGet, (void**)&o_clipGet },
    { "do_send_format_list",         { "Clipboard::do_send_format_list", "do_send_format_list: send_request failed", nullptr, nullptr }, (void*)h_clipSendFmt, (void**)&o_clipSendFmt },
    { "handle_clipboard_request",    { "Clipboard::handle_clipboard_request", "Received auto_save_complete: total=", nullptr, nullptr }, (void*)h_clipReq, (void**)&o_clipReq },
    { "setConnectInfo",              { "ControlConnectionSession::setConnectInfo", "startConnectOtherDevice, device_id: ", nullptr, nullptr }, (void*)h_setConnInfo, (void**)&o_setConnInfo },
    { "closeControlConnect",         { "ControlConnectionSession::closeControlConnect", nullptr, nullptr, nullptr }, (void*)h_closeConn, (void**)&o_closeConn },
    { "exitRoom",                    { "ControlConnectionSession::exitRoom", nullptr, nullptr, nullptr }, (void*)h_exitRoom, (void**)&o_exitRoom },
    { "VideoMainWindow::ctor",       { "home_control_session_start: window_created, device_id=", nullptr, nullptr, nullptr }, (void*)h_vmwCtor, (void**)&o_vmwCtor },
    { "updateCursor",                { "VideoUi::VideoWidget::updateCursor", "set cursor by id", "Default set arrow cursor", nullptr }, (void*)h_updateCursor, (void**)&o_updateCursor },
    { "startRemoteAssist",            { "NewUi::HomePageContent::startRemoteAssist", "startRemoteAssist: self is controlled, minimize controlled window", nullptr, nullptr }, (void*)h_startRemoteAssist, (void**)&o_startRemoteAssist },
};

// 4.36 删除了绝大多数“类名::方法名”日志，并重构了数个函数原型。
// 这里只保留经 4.36.0.9155 反编译确认过、原型仍与 detour 一致的入口。
static const hookset::Hook k436Hooks[] = {
    { "GamepadManager::Connect",      { "gamepad_connection result=connected count=", nullptr, nullptr, nullptr }, (void*)h_gamepadConnect, (void**)&o_gamepadConnect },
    { "GamepadManager::Disconnect",   { "gamepad_connection result=disconnected count=", nullptr, nullptr, nullptr }, (void*)h_gamepadDisconnect, (void**)&o_gamepadDisconnect },
    { "on_clipboard_update",          { "clipboard_update result=forwarded role=", nullptr, nullptr, nullptr }, (void*)h_clipUpdate, (void**)&o_clipUpdate },
    { "do_handle_format_list_request",{ "clipboard_format_list result=received request_id=", nullptr, nullptr, nullptr }, (void*)h_clipFmtList, (void**)&o_clipFmtList },
    { "do_send_format_list",          { "clipboard_format_list result=sent format_count=", nullptr, nullptr, nullptr }, (void*)h_clipSendFmt, (void**)&o_clipSendFmt },
    { "handle_clipboard_request",     { "Received auto_save_complete: total=", nullptr, nullptr, nullptr }, (void*)h_clipReq, (void**)&o_clipReq },
    { "VideoMainWindow::ctor",        { "home_control_session_start state=window_created", nullptr, nullptr, nullptr }, (void*)h_vmwCtor436, (void**)&o_vmwCtor436 },
    { "updateCursor",                 { "[CursorDiag] action=cursor_applied kind=arrow source=custom_build_failed", nullptr, nullptr, nullptr }, (void*)h_updateCursor436, (void**)&o_updateCursor436 },
};

void install_hooks(uintptr_t base) {
    if (MH_Initialize() != MH_OK) { uu_log("MH_Initialize failed"); return; }
    std::wstring vs = cfg::exe_version();
    const ver::VerSet& V = ver::pick(vs.c_str());
    g_verKnown = (!vs.empty() && vs == V.version);
    g_gvBase = base;
    g_gvVersion = vs.empty() ? L"?" : vs;
    const bool claims436 = g_verKnown && vs == L"4.36.0.9155";
    CCS_DEVICE_ID_OFF = V.deviceIdOff;
    g_isCtrlMemberOff = V.isCtrlMemberOff;
    g_isCtrlMemberEmbedded = V.isCtrlMemberEmbedded;
    ISCTRL_VT_SLOT_OFF = V.isCtrlVtSlotOff;
    VMW_DEVICE_ID_OFF = V.vmwDevIdOff;
    VMW_TITLE_OFF     = V.vmwTitleOff;
    uu_log("GameViewer version=%ls known=%d", vs.empty() ? L"?" : vs.c_str(), (int)g_verKnown);
    resolver::ModRange r{};
    resolver::get_ranges((HMODULE)base, r);
    const bool is436 = claims436
                    && r.img_end - r.img_beg == 0x412c000
                    && resolver::find_string(r, "clipboard_update result=forwarded role=")
                    && resolver::find_string(r, "startRemoteAssist: device data is not init, return");
    if (claims436 && !is436) {
        uu_log("GameViewer: 4.36 layout guard mismatch, refusing version-specific RVA hooks");
        return;
    }
    if (!is436) {
        uintptr_t scfn = resolver::find_func(r, {"ControlConnectionSession::setConnectInfo", "startConnectOtherDevice, device_id: "});
        uintptr_t d = derive_off_after_str(r, scfn, "startConnectOtherDevice, device_id: ");
        if (d) { CCS_DEVICE_ID_OFF = d; g_devIdAuto = true; }
        uu_log("deviceIdOff: table=%llu derived=%llu use=%llu", (unsigned long long)V.deviceIdOff,
               (unsigned long long)d, (unsigned long long)CCS_DEVICE_ID_OFF);
    } else {
        uu_log("deviceIdOff: 4.36 table=%llu (verified from VideoModel methods)",
               (unsigned long long)CCS_DEVICE_ID_OFF);
    }
    InProcRecorder rec;
    if (is436)
        hookset::install(r, k436Hooks, (int)(sizeof(k436Hooks) / sizeof(k436Hooks[0])), rec);
    else
        hookset::install(r, kLegacyHooks, (int)(sizeof(kLegacyHooks) / sizeof(kLegacyHooks[0])), rec);
    // 4.36+：输入发送改为会话级包装函数（vtable 槽 thunk → jmp 存根 → 包装）。
    //   这些包装函数无字符串锚点，只能按已知版本 RVA 安装；a1=session。
    //   旧版本 send*Rva=0，走上面的字符串锚点解析。
    if (g_verKnown && (V.sendKeyRva || V.sendMouseRva || V.sendWheelRva)) {
        if (V.sendKeyRva)   hookset::install_at((void*)(base + V.sendKeyRva),   "sendKeyboardEvent", "rva", (void*)h_sendKey,   (void**)&o_sendKey,   rec);
        if (V.sendMouseRva) hookset::install_at((void*)(base + V.sendMouseRva), "sendMouseEvent",    "rva", (void*)h_sendMouse, (void**)&o_sendMouse, rec);
        if (V.sendWheelRva) hookset::install_at((void*)(base + V.sendWheelRva), "sendMouseWheel",    "rva", (void*)h_sendWheel, (void**)&o_sendWheel, rec);
    }
    if (is436) {
        hookset::install_at((void*)(base + V.captureMouseRva), "captureMouse", "rva",
                            (void*)h_capture436, (void**)&o_capture436, rec);
        hookset::install_at((void*)(base + V.clipGetRva), "get_clipboard_data", "rva",
                            (void*)h_clipGet436, (void**)&o_clipGet436, rec);
        hookset::install_at((void*)(base + V.cursorApplyRva), "VideoWidget::applyCursor", "rva",
                            (void*)h_cursorApply436, (void**)&o_cursorApply436, rec);
        g_isCtrlConnectRet436 = base + V.isCtrlConnectRetRva;
        g_isCtrlGuardRet436 = base + V.isCtrlGuardRetRva;
        hookset::install_at((void*)(base + V.isCtrlNarrowRva), "isControlledConnectOnly", "rva",
                            (void*)h_isCtrlNarrow436, (void**)&o_isCtrlNarrow436, rec);
        g_deviceControlAllowedOff436 = V.deviceControlAllowedOff;
        g_deviceControlledOff436 = V.deviceControlledOff;
        hookset::install_at((void*)(base + V.deviceSceneRenderRva), "deviceDesktopControlAllowed", "rva",
                            (void*)h_deviceSceneRender436, (void**)&o_deviceSceneRender436, rec);
        hookset::install_at((void*)(base + V.vmwCloseEventRva), "VideoMainWindow::closeEvent", "rva",
                            (void*)h_vmwClose436, (void**)&o_vmwClose436, rec);
        hookset::install_at((void*)(base + V.vmwDtorRva), "VideoMainWindow::~VideoMainWindow", "rva",
                            (void*)h_vmwDtor436, (void**)&o_vmwDtor436, rec);
    }
    uintptr_t isCtrlWrapper = g_verKnown ? base + V.isCtrlWrapperRva : 0;
    if (V.isCtrlWrapperRva && isCtrlWrapper >= r.text_beg && isCtrlWrapper < r.text_end)
        hookset::install_at((void*)isCtrlWrapper, "isControlledWrapper", "rva",
                            (void*)h_isCtrlWrapper, (void**)&o_isCtrlWrapper, rec);
    else
        rec.record("isControlledWrapper", nullptr, "rva", false);
    uintptr_t isCtrlDirect = g_verKnown ? base + V.isCtrlDirectRva : 0;
    if (V.isCtrlDirectRva && isCtrlDirect >= r.text_beg && isCtrlDirect < r.text_end)
        hookset::install_at((void*)isCtrlDirect, "isControlledDirect", "rva",
                            (void*)h_isCtrlWrapper, (void**)&o_isCtrlDirect, rec);
    hookset::install_export(L"user32.dll", "LockWorkStation", (void*)h_lockWorkStation, (void**)&o_lockWorkStation, rec);
    hookset::install_export(L"user32.dll", "ClipCursor", (void*)h_ClipCursor, (void**)&o_ClipCursor, rec);
    if (!is436) {
        uintptr_t hke = resolver::find_func(r, { "VideoUi::VideoWidget::handleKeyEvent",
                                                 "handleKeyEvent: controller shortcut handled" });
        void* fwd = find_raw_key_forward(r, hke);
        hookset::install_at(fwd, "rawKeyForward", "str", (void*)h_rawKeyForward, (void**)&o_rawKeyForward, rec);
    }
    uu_log("install_hooks done");
}

DebugInfo debug_snapshot() {
    std::lock_guard<std::mutex> lk(g_dbgMtx);
    DebugInfo d;
    d.gvVersion = g_gvVersion;
    d.gvKnown = g_verKnown;
    d.devIdOff = CCS_DEVICE_ID_OFF;
    d.vmwDevIdOff = VMW_DEVICE_ID_OFF;
    d.vmwTitleOff = VMW_TITLE_OFF;
    d.devIdAuto = g_devIdAuto;
    d.vmwAuto = g_vmwOffAuto;
    d.serverRunning = false;
    for (const auto& h : g_hookStats)
        d.hooks.push_back({ L"ctl", h.name, h.how, h.addr ? (unsigned long long)((uintptr_t)h.addr - g_gvBase) : 0, h.ok });
    if (HANDLE sm = OpenFileMappingW(FILE_MAP_READ, FALSE, srvdbg::MAP_NAME)) {
        if (auto* sh = (srvdbg::Shared*)MapViewOfFile(sm, FILE_MAP_READ, 0, 0, sizeof(srvdbg::Shared))) {
            int n = (int)sh->count; if (n < 0) n = 0; if (n > srvdbg::MAX_HOOKS) n = srvdbg::MAX_HOOKS;
            for (int i = 0; i < n; ++i) {
                const srvdbg::Entry& e = sh->hooks[i];
                char name[srvdbg::NAME_LEN]; lstrcpynA(name, e.name, srvdbg::NAME_LEN);
                char how[8];                 lstrcpynA(how,  e.how,  sizeof(how));
                d.hooks.push_back({ L"srv", std::string(name), std::string(how), e.off, e.ok != 0 });
            }
            d.serverRunning = true;
            UnmapViewOfFile(sh);
        }
        CloseHandle(sm);
    }
    return d;
}
