#pragma once
#include <cstdint>
#include <wchar.h>

namespace ver {

// 4.40 只保留「被控期间仍可远控其他主机」。观看/仅浏览走官方能力。

// 4.42.1.2835 定位（IDA，SizeOfImage=0x4636000）：
//   HomePageContent 次虚表 0x143B53670，槽 +0xE0 是 thunk 0x1400BA9F6 -> sub_1402CEA70
//   （movzx eax,[rcx+0FAh]; ret，8 字节，可安全挂钩）
//   命令分发器 sub_1402D7B90(this, logname, callback)：被控时不跑 callback，转去收起被控窗口。
//   call [rax+0E0h] @ 0x2D7BD7，返回地址 0x2D7BDD。startRemoteAssist / startCloudDeviceAdd /
//   startCloudDeviceMarket 三个命令都走它，所以 connect / guard 两个返回值相同。
//   DeviceDesktopScene::render @ 0x3FF0B0：桌面区读 +0x69 / +0x6A；
//   外层用 +0x8C 决定是否画 a1+120 操作条，条内再读 +0x8D / +0x8E。
//   底部工具栏（观看模式/文件传输等）走 +0xD8 的 8 字节工具数组，不看 +0x8D。
// 4.40 定位（旧版保留）：isControlled @ 0x2C95E0，dispatcher ret @ 0x2D270D，render @ 0x3F96B0。
struct VerSet {
    const wchar_t* version;
    // HomePageContent 次对象上的 isControlled()：读 this+0xFA 的一字节。
    // 虚表槽 0xE0，实现经 thunk 跳到这里。
    uintptr_t isCtrlNarrowRva;
    // 命令分发器 HomePageContent 包装（startRemoteAssist 等）里
    // call [vtable+0xE0] 之后的返回地址。只在这两处撒谎，收起被控页仍用真值。
    uintptr_t isCtrlConnectRetRva, isCtrlGuardRetRva;
    // DeviceDesktopScene::render(DeviceDetailViewData const&)
    uintptr_t deviceSceneRenderRva;
    // DeviceDetailViewData：platform 在 +0x60。
    // desktopAllowed=1 才不会画「该设备不允许被控」；desktopControlled=1 会藏进入桌面按钮。
    // actionAllowed=0 时 4.40 操作区不可用；4.42.1 起外层门是 +0x8C（render 用来 show/hide a1+120）。
    // 4.42.1 的 +0x8D 是该条启用样式（1=style 5，0=style 16），用 deviceActionEnabledOff 置 1。
    // +0x8E 是能力位，清零会把操作条关掉，所以 deviceActionControlledOff=0 表示跳过。
    // 4.42.1 底部工具栏：+0xD8/+0xE0 是 8 字节项 vector（DWORD toolId, +4 启用, +5 上栏）。
    // 被控时 +4 被清成 0，DesktopBottomWidget 把按钮画成灰的；+5 决定该项出不出现。
    // hook 只置 +4=1，出栏集合保持官方，布局不动。4.40 填 0 跳过。
    uintptr_t deviceDesktopAllowedOff, deviceDesktopControlledOff;
    uintptr_t deviceActionAllowedOff, deviceActionControlledOff;
    uintptr_t deviceActionEnabledOff, deviceBottomToolsVecOff;
    // DesktopButton 的启用状态设置（setEnabled 语义：a2=0 置 state 3=禁用）。
    // 被控时数据层走这里把底栏按钮打成禁用，按钮就点不动了（DesktopButton::event
    // 里 state==3 直接跳过鼠标事件）。hook 成恒为启用即可。4.40 填 0 跳过。
    uintptr_t deviceBottomButtonStateRva;
    // DesktopButton 的点击动作（只做诊断：确认鼠标事件有没有走到这里）。
    uintptr_t deviceBottomButtonClickRva;
    uintptr_t imageSize;
};

inline const VerSet kVer[] = {
    // GameViewer 4.42.1.2835  SizeOfImage=0x4636000
    { L"4.42.1.2835", 0x2CEA70, 0x2D7BDD, 0x2D7BDD, 0x3FF0B0, 0x69, 0x6a, 0x8c, 0, 0x8d, 0xd8, 0x6B6D80, 0x6B6920, 0x4636000 },
    // GameViewer 4.40.1.2090  SizeOfImage=0x4570000
    // RVA 与 4.40.0.1780 相同：isControlled @ 0x2C95E0，dispatcher ret @ 0x2D270D，render @ 0x3F96B0
    { L"4.40.1.2090", 0x2C95E0, 0x2D270D, 0x2D270D, 0x3F96B0, 0x69, 0x6a, 0x8f, 0x90, 0, 0, 0, 0, 0x4570000 },
    // GameViewer 4.40.0.1780  SizeOfImage=0x4570000
    { L"4.40.0.1780", 0x2C95E0, 0x2D270D, 0x2D270D, 0x3F96B0, 0x69, 0x6a, 0x8f, 0x90, 0, 0, 0, 0, 0x4570000 },
};

inline const VerSet& pick(const wchar_t* v) {
    if (v) for (auto& s : kVer) if (!wcscmp(s.version, v)) return s;
    return kVer[0];
}

}
