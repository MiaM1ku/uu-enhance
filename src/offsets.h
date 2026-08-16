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
    // 4.36+: 底层输入发送入口无字符串锚点，按已知版本 RVA 安装。
    uintptr_t sendKeyRva, sendMouseRva, sendWheelRva;
    // 4.37 在上层输入入口记录当前 VideoWidget，仅把它作为会话键传给底层 Hook，
    // 不跳过原程序的事件收尾。mouseContent 是 VideoContentWindow 的第二条鼠标路径。
    uintptr_t inputKeyRva, inputMouseRva, inputWheelRva, inputMouseContentRva;
    // 4.36+ 重构后的鼠标捕获入口、剪贴板数据读取函数，以及最终应用/隐藏光标的入口。
    uintptr_t captureMouseRva, clipGetRva, cursorApplyRva;
    // 4.36+ 只在“提交连接”和“发起远控保护”两个调用点忽略 isControlled()。
    // 其它调用点保留真实结果，确保本机被控页仍可正常收起。
    uintptr_t isCtrlNarrowRva, isCtrlConnectRetRva, isCtrlGuardRetRva;
    // 4.36+ 的设备桌面渲染入口，以及 DeviceDetailViewData 中两组控制状态。
    // desktop* 控制“进入桌面”区域，action* 控制快捷操作按钮；4.37 两组都要覆盖。
    uintptr_t deviceSceneRenderRva;
    uintptr_t deviceDesktopAllowedOff, deviceDesktopControlledOff;
    uintptr_t deviceActionAllowedOff, deviceActionControlledOff;
    // 4.36+ VideoMainWindow 的关闭事件与析构函数。4.37 另在 VideoWidget::close
    // 按输入会话使用的同一对象指针清理，覆盖断开后窗口仍存活的情况。
    uintptr_t vmwCloseEventRva, vmwDtorRva, videoWidgetCloseRva;
    // 对已知现代版本同时校验 SizeOfImage，避免相同文件版本的热更新误装 RVA hook。
    uintptr_t imageSize;
};

inline const VerSet kVer[] = {
    // VideoMainWindow: +488=device id, +496=connected session title, +512=cover URL.
    { L"4.37.0.9232", 264, 488, 496, 0x5bac50, 0, 0, false, 0,
      0xe49650, 0xe496d0, 0xe49780, 0x5588b0, 0x558de0, 0x55a5c0, 0x4f0fd0,
      0x5540d0, 0xdc20f0, 0x5523f0,
      0x22b840, 0x23a161, 0x234119, 0x356000, 0x69, 0x6a, 0x8f, 0x90,
      0x517930, 0x50df80, 0x557580, 0x4113000 },
    // VideoMainWindow: +480=device id, +488=connected session title.
    // +504 is the desktop cover-image URL and must never be used as a title.
    { L"4.36.0.9155", 4296, 480, 488, 0x5ddac0, 0, 0, false, 0,
      0xe394e0, 0xe39560, 0xe39610, 0, 0, 0, 0,
      0x577060, 0xdc9ad0, 0x575330,
      0x22c570, 0x23b121, 0x2350a9, 0x354ba0, 0x69, 0x6a, 0, 0,
      0x539810, 0x52fe30, 0, 0x412c000 },
    { L"4.33.0.8907", 4296, 344, 352, 0x73fe10, 0x4ea310, 0x30, true, 0x140,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { L"4.31.1.8795", 4296, 344, 352, 0x73fe20, 0x4e4d30, 0x30, true, 0x140,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { L"4.29.0.8620", 4296, 344, 352, 0x32489, 0, 0x130, false, 0x140,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { L"4.26.0.8259", 3984, 344, 352, 0, 0, 0, false, 0x140,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },   // vmw offsets unverified
};

inline const VerSet& pick(const wchar_t* v) {
    if (v) for (auto& s : kVer) if (!wcscmp(s.version, v)) return s;
    return kVer[0];
}

}
