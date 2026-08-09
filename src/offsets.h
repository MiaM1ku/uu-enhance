#pragma once
#include <cstdint>
#include <cwchar>

namespace ver {

struct VerSet {
    const wchar_t* version;
    uintptr_t deviceIdOff;
    uintptr_t vmwDevIdOff, vmwTitleOff;
    uintptr_t isCtrlWrapperRva, isCtrlDirectRva;
    uintptr_t isCtrlMemberOff;
    bool isCtrlMemberEmbedded;
    // 4.36+: isControlled() 虚函数在状态对象 vtable 内的字节偏移
    //   (4.33 及更早为 0x140，4.36 起移到 0xF0)
    uintptr_t isCtrlVtSlotOff;
    // 4.36+: 输入发送改走会话级包装函数(无字符串锚点)，按已知版本 RVA 安装。
    //   旧版本保持 0，由旧版 kHooks 的字符串锚点解析。
    uintptr_t sendKeyRva, sendMouseRva, sendWheelRva;
    // 4.36 重构后的鼠标捕获入口、剪贴板数据读取函数，以及最终应用/隐藏光标的入口。
    uintptr_t captureMouseRva, clipGetRva, cursorApplyRva;
    // 4.36 只在“提交连接”和“发起远控保护”两个调用点忽略 isControlled()。
    // 其它调用点保留真实结果，确保本机被控页仍可正常收起。
    uintptr_t isCtrlNarrowRva, isCtrlConnectRetRva, isCtrlGuardRetRva;
    // 4.36 的设备桌面渲染入口，以及 DeviceDetailViewData 中“允许发起控制”与
    // “本机正被控制”标志偏移。二者任一不满足都会使“进入桌面”置灰。
    uintptr_t deviceSceneRenderRva, deviceControlAllowedOff, deviceControlledOff;
    // 4.36 VideoMainWindow 的关闭事件与析构函数，用于及时清理托盘会话。
    uintptr_t vmwCloseEventRva, vmwDtorRva;
};

inline const VerSet kVer[] = {
    // VideoMainWindow: +480=device id, +488=connected session title.
    // +504 is the desktop cover-image URL and must never be used as a title.
    { L"4.36.0.9155", 4296, 480, 488, 0x5ddac0, 0, 0, false, 0,
      0xe394e0, 0xe39560, 0xe39610, 0x577060, 0xdc9ad0, 0x575330,
      0x22c570, 0x23b121, 0x2350a9, 0x354ba0, 0x69, 0x6a,
      0x539810, 0x52fe30 },
    { L"4.33.0.8907", 4296, 344, 352, 0x73fe10, 0x4ea310, 0x30, true, 0x140,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { L"4.31.1.8795", 4296, 344, 352, 0x73fe20, 0x4e4d30, 0x30, true, 0x140,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { L"4.29.0.8620", 4296, 344, 352, 0x32489, 0, 0x130, false, 0x140,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { L"4.26.0.8259", 3984, 344, 352, 0, 0, 0, false, 0x140,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },   // vmw offsets unverified
};

inline const VerSet& pick(const wchar_t* v) {
    if (v) for (auto& s : kVer) if (!wcscmp(s.version, v)) return s;
    return kVer[0];
}

}
