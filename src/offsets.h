#pragma once
#include <cstdint>
#include <cwchar>

// 按版本的地址表。每个目标含写死 RVA + 一段函数开头的字节特征(AOB，rip 相对位移用 ?? 通配)。
// 定位优先级见 resolver / hooks.cpp：多锚点字符串 → AOB → 这里的 RVA → 跳过。
// 适配新版本：在 kVer 里加一行即可(RVA 用 IDA 重新核对，AOB 重新抓函数开头字节)。
namespace ver {

struct Target { uintptr_t rva; const char* aob; };

struct VerSet {
    const wchar_t* version;
    uintptr_t deviceIdOff;          // CCS 内 device_id std::string 偏移
    Target sendMouse, sendWheel, sendKey, enableCapture, updateCursor;
    Target gpConnect, gpDisconnect, gpUpdate;
    Target clipUpdate, clipFmtList, clipGet, clipSendFmt;
    Target setConnInfo, closeConn, exitRoom;
    Target startRemoteAssist;  // HomePageContent::startRemoteAssist — 被控时绕过主控限制
    Target isCtrlGlobalRva;    // .data 全局单例指针 RVA；这是数据槽，通常不提供函数型 AOB
    bool isCtrlGlobalDirect;   // true: 全局槽直接指向状态对象；false: 从 manager+0x30 取状态对象
    bool isCtrlMemberEmbedded; // true: HomePageContent+0x30 是内嵌对象；false: 该字段是对象指针
    Target initDevStatus;      // DesktopImageWidget::initDeviceStatus — 被控时懒初始化 isControlled() hook
    Target desktopCheck;       // initDeviceStatus 内部调用的可控性检查：返回 false=禁用，true=启用；被控时强制返回 true
    Target isCtrlWrapper;      // fcn.140032489 包装器：调用 isControlled() 虚函数；被控时强制返回 false 以恢复按钮
};

inline const VerSet kVer[] = {
    { L"4.31.1.8795", 0x10c8,
      /*sendMouse   */ { 0x8c68b0, "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 80 FB FF FF 48 81 EC 80 05 00 00 48" },
      /*sendWheel   */ { 0x8c73f0, "48 89 5C 24 18 48 89 74 24 20 55 57 41 54 41 56 41 57 48 8D AC 24 D0 FD FF FF 48 81 EC 30 03 00 00 48 8B 05 ?? ?? ?? ??" },
      /*sendKey     */ { 0x8c4e80, "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 D0 FD FF FF 48 81 EC 30 03 00 00 48 8B 05 ?? ?? ?? ?? 48 33" },
      /*enableCap   */ { 0x6b10e0, "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 40 FD FF FF 48 81 EC C0 03 00 00 48 8B 05 ?? ?? ?? ?? 48 33" },
      /*updateCursor*/ { 0x6bb3c0, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8D 6C 24 90 48 81 EC 70 01 00 00 44 0F B6 F2" },
      /*gpConnect   */ { 0xa64790, "48 89 5C 24 18 48 89 74 24 20 57 48 81 EC 90 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 88 01 00 00 48 8B FA 48" },
      /*gpDisconnect*/ { 0xa64c40, "48 89 5C 24 18 48 89 74 24 20 57 48 81 EC 90 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 88 01 00 00 48 8B FA 48" },
      /*gpUpdate    */ { 0xa68e80, "40 53 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 48 48 8B C2 48 8B D9 FF 05 ?? ?? ?? ?? 48 8D 54 24 28 48 8B" },
      /*clipUpdate  */ { 0x92e790, "48 89 5C 24 10 55 48 8D AC 24 60 FF FF FF 48 81 EC A0 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 90 00 00 00 48 8B" },
      /*clipFmtList */ { 0x9242c0, "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 40 FF FF FF 48 81 EC C0 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 B8 00 00 00" },
      /*clipGet     */ { 0x9261b0, "40 55 53 56 57 41 54 41 56 41 57 48 8D AC 24 C0 FE FF FF 48 81 EC 40 02 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 30 01 00 00 49 8B D8 44 8B" },
      /*clipSendFmt */ { 0x924ed0, "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 60 FF FF FF 48 81 EC A0 01 00 00 48 8B F1 45 33 C0 33 D2 48 8D 4C 24 50 E8 ?? ?? ?? ??" },
      /*setConnInfo */ { 0x8cc650, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8D 6C 24 90 48 81 EC 70 01 00 00 4C 8B F2 48 8B F1 E8" },
      /*closeConn   */ { 0x89c600, "48 89 5C 24 08 57 48 81 EC 70 01 00 00 48 8B F9 83 A1 38 12 00 00 FE E8 ?? ?? ?? ?? 48 8B D8 48 8D 05 ?? ?? ?? ?? 48 89" },
      /*exitRoom    */ { 0x8a4d70, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8D 6C 24 90 48 81 EC 70 01 00 00 48 8B D9 E8 ?? ?? ??" },
      /*startRemoteAssist*/ { 0x4b31d0, "40 55 53 56 57 41 56 48 8D AC 24 30 FF FF FF 48 81 EC D0 01 00 00 48 8B F1 80 B9 C1 00 00 00 00" },
      /*isCtrlGlobalRva  */ { 0x44c30b0, nullptr },
      /*isCtrlGlobalDirect*/ false,
      /*isCtrlMemberEmbedded*/ true,
      /*initDevStatus    */ { 0x418670, "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 20 FF FF FF 48 81 EC E0 01 00 00" },
      /*desktopCheck     */ { 0x404900, "40 53 48 83 EC 20 8B 41 28 83 E8 03 83 F8 03 76 45 48 8B 41 58 48 8D 59 58 83 78 04 00 74 37 48" },
      /*isCtrlWrapper    */ { 0x73fe20, "48 83 EC 28 E8 ?? ?? ?? ?? 48 85 C0 74 17 48 8B 10 48 8B C8 FF 92 40 01 00 00 84 C0 74 07 B0 01 48 83 C4 28 C3 32 C0 48 83 C4 28 C3" },
    },
    { L"4.29.0.8620", 0x10c8,
      /*sendMouse   */ { 0x8bce10, "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 80 FB FF FF 48 81 EC 80 05 00 00 48" },
      /*sendWheel   */ { 0x8bd950, "48 89 5C 24 18 48 89 74 24 20 55 57 41 54 41 56 41 57 48 8D AC 24 D0 FD FF FF 48 81 EC 30 03 00 00 48 8B 05 ?? ?? ?? ??" },
      /*sendKey     */ { 0x8bb3e0, "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 D0 FD FF FF 48 81 EC 30 03 00 00 48 8B 05 ?? ?? ?? ?? 48 33" },
      /*enableCap   */ { 0x6aa770, "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 40 FD FF FF 48 81 EC C0 03 00 00 48 8B 05 ?? ?? ?? ?? 48 33" },
      /*updateCursor*/ { 0x6b49e0, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8D 6C 24 90 48 81 EC 70 01 00 00 44 0F B6 F2" },
      /*gpConnect   */ { 0xa5aa70, "48 89 5C 24 18 48 89 74 24 20 57 48 81 EC 90 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 88 01 00 00 48 8B FA 48" },
      /*gpDisconnect*/ { 0xa5af20, "48 89 5C 24 18 48 89 74 24 20 57 48 81 EC 90 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 88 01 00 00 48 8B FA 48" },
      /*gpUpdate    */ { 0xa5f160, "40 53 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 48 48 8B C2 48 8B D9 FF 05 ?? ?? ?? ?? 48 8D 54 24 28 48 8B" },
      /*clipUpdate  */ { 0x924a30, "48 89 5C 24 10 55 48 8D AC 24 60 FF FF FF 48 81 EC A0 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 90 00 00 00 48 8B" },
      /*clipFmtList */ { 0x91a560, "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 40 FF FF FF 48 81 EC C0 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 B8 00 00 00" },
      /*clipGet     */ { 0x91c450, "40 55 53 56 57 41 54 41 56 41 57 48 8D AC 24 C0 FE FF FF 48 81 EC 40 02 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 30 01 00 00 49 8B D8 44 8B" },
      /*clipSendFmt */ { 0x91b170, "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 60 FF FF FF 48 81 EC A0 01 00 00 48 8B F1 45 33 C0 33 D2 48 8D 4C 24 50 E8 ?? ?? ?? ??" },
      /*setConnInfo */ { 0x8c2bb0, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8D 6C 24 90 48 81 EC 70 01 00 00 4C 8B F2 48 8B F1 E8" },
      /*closeConn   */ { 0x892b70, "48 89 5C 24 08 57 48 81 EC 70 01 00 00 48 8B F9 83 A1 38 12 00 00 FE E8 ?? ?? ?? ?? 48 8B D8 48 8D 05 ?? ?? ?? ?? 48 89" },
      /*exitRoom    */ { 0x89b2e0, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8D 6C 24 90 48 81 EC 70 01 00 00 48 8B D9 E8 ?? ?? ??" },
      /*startRemoteAssist*/ { 0x4ad940, "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B F1 48 8B 89 30 01 00 00 48 85 C9 74 08 48 8B 01 FF 90 40 01 00 00" },
      /*isCtrlGlobalRva  */ { 0x44a82f0, nullptr },
      /*isCtrlGlobalDirect*/ false,
      /*isCtrlMemberEmbedded*/ false,
      /*initDevStatus    */ { 0x415100, "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 20 FF FF FF 48 81 EC E0 01 00 00" },
      /*desktopCheck     */ { 0x401390, "40 53 48 83 EC 20 8B 41 28 83 E8 03 83 F8 03 76 45 48 8B 41 58 48 8D 59 58 83 78 04 00 74 37 48" },
      /*isCtrlWrapper    */ { 0x32489,  "48 83 EC 28 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 13 48 8B 49 30 48 85 C9 74 0A 48 8B 01 FF 90 40 01 00 00 48 83 C4 28 C3 32 C0 48 83 C4 28 C3" },
    },
    { L"4.26.0.8259", 3984,
      /*sendMouse   */ { 0x862080, "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 80 FB FF FF 48 81 EC 80 05 00 00 48" },
      /*sendWheel   */ { 0x862bc0, "48 89 5C 24 18 48 89 74 24 20 55 57 41 54 41 56 41 57 48 8D AC 24 D0 FD FF FF 48 81 EC 30 03 00 00 48 8B 05 ?? ?? ?? ??" },
      /*sendKey     */ { 0x860650, "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 D0 FD FF FF 48 81 EC 30 03 00 00 48 8B 05 ?? ?? ?? ?? 48 33" },
      /*enableCap   */ { 0x6639e0, "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 40 FD FF FF 48 81 EC C0 03 00 00 48 8B 05 ?? ?? ?? ?? 48 33" },
      /*updateCursor*/ { 0x66dba0, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8D 6C 24 90 48 81 EC 70 01 00 00 44 0F B6 F2" },
      /*gpConnect   */ { 0x9ea770, "48 89 5C 24 18 48 89 74 24 20 57 48 81 EC 90 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 88 01 00 00 48 8B FA 48" },
      /*gpDisconnect*/ { 0x9eac20, "48 89 5C 24 18 48 89 74 24 20 57 48 81 EC 90 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 88 01 00 00 48 8B FA 48" },
      /*gpUpdate    */ { 0x9eeeb0, "40 53 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 48 48 8B C2 48 8B D9 FF 05 ?? ?? ?? ?? 48 8D 54 24 28 48 8B" },
      /*clipUpdate  */ { 0x8c5280, "48 89 5C 24 10 55 48 8D AC 24 60 FF FF FF 48 81 EC A0 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 90 00 00 00 48 8B" },
      /*clipFmtList */ { 0x8badb0, "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 40 FF FF FF 48 81 EC C0 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 B8 00 00 00" },
      /*clipGet     */ { 0x8bcca0, "40 55 53 56 57 41 54 41 56 41 57 48 8D AC 24 C0 FE FF FF 48 81 EC 40 02 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 30 01 00 00 49 8B D8 44 8B" },
      /*clipSendFmt */ { 0x8bb9c0, "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 60 FF FF FF 48 81 EC A0 01 00 00 48 8B F1 45 33 C0 33 D2 48 8D 4C 24 50 E8 ?? ?? ?? ??" },
      /*setConnInfo */ { 0x867ce0, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8D 6C 24 90 48 81 EC 70 01 00 00 4C 8B F2 48 8B F1 E8" },
      /*closeConn   */ { 0x83a3f0, "48 89 5C 24 08 57 48 81 EC 70 01 00 00 48 8B F9 83 A1 00 11 00 00 FE E8 ?? ?? ?? ?? 48 8B D8 48 8D 05 ?? ?? ?? ?? 48 89" },
      /*exitRoom    */ { 0x841b90, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8D 6C 24 90 48 81 EC 70 01 00 00 48 8B D9 E8 ?? ?? ??" },
      /*startRemoteAssist*/ { 0, nullptr },
      /*isCtrlGlobalRva  */ { 0, nullptr },
      /*isCtrlGlobalDirect*/ false,
      /*isCtrlMemberEmbedded*/ false,
      /*initDevStatus    */ { 0, nullptr },
      /*desktopCheck     */ { 0, nullptr },
      /*isCtrlWrapper    */ { 0, nullptr },
    },
};

inline const VerSet& pick(const wchar_t* v) {
    if (v) for (auto& s : kVer) if (!wcscmp(s.version, v)) return s;
    return kVer[0];   // 未知版本用基线兜底(RVA 多半不对，但字符串/AOB 会先生效)
}

} // namespace ver
